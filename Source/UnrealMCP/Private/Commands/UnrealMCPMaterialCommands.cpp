#include "Commands/UnrealMCPMaterialCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "MaterialEditingLibrary.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionExternalCodeBase.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionRerouteBase.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "RHIShaderPlatform.h"
#include "SceneTypes.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"

namespace
{
struct FMaterialGraphTarget
{
    UObject* Asset = nullptr;
    UMaterial* Material = nullptr;
    UMaterialFunction* Function = nullptr;

    bool IsValid() const
    {
        return Material != nullptr || Function != nullptr;
    }

    bool IsFunctionGraph() const
    {
        return Function != nullptr;
    }

    FString GetGraphType() const
    {
        return IsFunctionGraph() ? TEXT("function") : TEXT("material");
    }

    FString GetPathName() const
    {
        return Asset ? Asset->GetPathName() : FString();
    }

    TConstArrayView<TObjectPtr<UMaterialExpression>> GetExpressions() const
    {
        if (Material)
        {
            return Material->GetExpressions();
        }
        if (Function)
        {
            return Function->GetExpressions();
        }
        return TConstArrayView<TObjectPtr<UMaterialExpression>>();
    }

    bool HasUnconnectedCustomInputs() const
    {
        for (const TObjectPtr<UMaterialExpression>& ExpressionPtr : GetExpressions())
        {
            if (const UMaterialExpressionCustom* CustomExpression = Cast<UMaterialExpressionCustom>(ExpressionPtr.Get()))
            {
                for (const FCustomInput& CustomInput : CustomExpression->Inputs)
                {
                    if (!CustomInput.Input.Expression)
                    {
                        return true;
                    }
                }
            }
        }

        return false;
    }

    void MarkChanged(bool bPostEditChange = true) const
    {
        if (!Asset)
        {
            return;
        }

        Asset->Modify();
        Asset->MarkPackageDirty();
        if (bPostEditChange && !HasUnconnectedCustomInputs())
        {
            Asset->PostEditChange();
        }
    }
};

FString NormalizeObjectPathForLoad(const FString& ObjectPath)
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

FString NormalizeMaterialGraphType(const FString& GraphType)
{
    if (GraphType.Equals(TEXT("material_function"), ESearchCase::IgnoreCase) ||
        GraphType.Equals(TEXT("function"), ESearchCase::IgnoreCase))
    {
        return TEXT("function");
    }
    if (GraphType.Equals(TEXT("material"), ESearchCase::IgnoreCase))
    {
        return TEXT("material");
    }
    return TEXT("auto");
}

bool LoadMaterialGraphByObjectPath(const FString& GraphPath, const FString& GraphType, FMaterialGraphTarget& OutTarget, FString& OutError)
{
    const FString Query = NormalizeObjectPathForLoad(GraphPath);
    const FString NormalizedGraphType = NormalizeMaterialGraphType(GraphType);

    if (NormalizedGraphType != TEXT("function"))
    {
        if (UMaterial* Material = LoadObject<UMaterial>(nullptr, *Query))
        {
            OutTarget.Asset = Material;
            OutTarget.Material = Material;
            return true;
        }
    }

    if (NormalizedGraphType != TEXT("material"))
    {
        if (UMaterialFunction* Function = LoadObject<UMaterialFunction>(nullptr, *Query))
        {
            OutTarget.Asset = Function;
            OutTarget.Function = Function;
            return true;
        }
    }

    if (UMaterialInstance* MaterialInstance = LoadObject<UMaterialInstance>(nullptr, *Query))
    {
        OutError = FString::Printf(
            TEXT("Material Instance has no expression graph: %s. Use parameter tools instead."),
            *MaterialInstance->GetPathName());
    }

    return false;
}

void AppendAssetCandidatesForClass(UClass* AssetClass, const FString& GraphPathOrName, TArray<FString>& CandidatePaths)
{
    if (!AssetClass)
    {
        return;
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FAssetData> Assets;
    AssetRegistryModule.Get().GetAssetsByClass(AssetClass->GetClassPathName(), Assets, true);

    const FString Query = NormalizeObjectPathForLoad(GraphPathOrName);
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
            CandidatePaths.AddUnique(ObjectPath);
        }
    }
}

TArray<FString> FindMaterialGraphAssetPaths(const FString& GraphPathOrName, const FString& GraphType)
{
    TArray<FString> CandidatePaths;
    const FString NormalizedGraphType = NormalizeMaterialGraphType(GraphType);

    FMaterialGraphTarget DirectTarget;
    FString DirectError;
    if (LoadMaterialGraphByObjectPath(GraphPathOrName, NormalizedGraphType, DirectTarget, DirectError))
    {
        CandidatePaths.Add(DirectTarget.GetPathName());
        return CandidatePaths;
    }

    if (NormalizedGraphType != TEXT("function"))
    {
        AppendAssetCandidatesForClass(UMaterial::StaticClass(), GraphPathOrName, CandidatePaths);
    }
    if (NormalizedGraphType != TEXT("material"))
    {
        AppendAssetCandidatesForClass(UMaterialFunction::StaticClass(), GraphPathOrName, CandidatePaths);
    }

    return CandidatePaths;
}

bool ResolveMaterialGraph(const FString& GraphPath, const FString& GraphType, FMaterialGraphTarget& OutTarget, TArray<FString>* OutCandidates, FString& OutError)
{
    if (LoadMaterialGraphByObjectPath(GraphPath, GraphType, OutTarget, OutError))
    {
        return true;
    }

    const TArray<FString> CandidatePaths = FindMaterialGraphAssetPaths(GraphPath, GraphType);
    if (OutCandidates)
    {
        *OutCandidates = CandidatePaths;
    }

    if (CandidatePaths.Num() == 1)
    {
        OutError.Reset();
        return LoadMaterialGraphByObjectPath(CandidatePaths[0], GraphType, OutTarget, OutError);
    }

    if (OutError.IsEmpty())
    {
        if (CandidatePaths.Num() > 1)
        {
            OutError = FString::Printf(TEXT("Ambiguous material graph '%s'. Use a full object path. Candidates: %s"),
                *GraphPath,
                *FString::Join(CandidatePaths, TEXT(", ")));
        }
        else
        {
            OutError = FString::Printf(TEXT("Material or Material Function not found: %s"), *GraphPath);
        }
    }

    return false;
}

FProperty* FindPropertyByNameLoose(UObject* Object, const FString& PropertyName)
{
    if (!Object)
    {
        return nullptr;
    }

    if (FProperty* Property = Object->GetClass()->FindPropertyByName(*PropertyName))
    {
        return Property;
    }

    for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
    {
        FProperty* Property = *It;
        if (Property && Property->GetName().Equals(PropertyName, ESearchCase::IgnoreCase))
        {
            return Property;
        }
    }

    return nullptr;
}

FString JsonValueToImportText(const TSharedPtr<FJsonValue>& Value)
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

bool TryGetObjectNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, double& OutValue)
{
    if (!Object.IsValid())
    {
        return false;
    }

    if (Object->TryGetNumberField(FieldName, OutValue))
    {
        return true;
    }

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
    {
        if (Pair.Key.Equals(FieldName, ESearchCase::IgnoreCase) && Pair.Value.IsValid() && Pair.Value->Type == EJson::Number)
        {
            OutValue = Pair.Value->AsNumber();
            return true;
        }
    }

    return false;
}

FString JsonValueToStructImportText(const TSharedPtr<FJsonValue>& Value, const FString& StructName)
{
    TArray<double> Numbers;
    if (Value.IsValid() && Value->Type == EJson::Array)
    {
        const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
        for (const TSharedPtr<FJsonValue>& ArrayValue : Array)
        {
            Numbers.Add(ArrayValue.IsValid() ? ArrayValue->AsNumber() : 0.0);
        }
    }
    else if (Value.IsValid() && Value->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        auto AddField = [&Numbers, &Object](const TCHAR* FieldName, double DefaultValue)
        {
            double Number = DefaultValue;
            TryGetObjectNumber(Object, FieldName, Number);
            Numbers.Add(Number);
        };

        if (StructName == TEXT("LinearColor") || StructName == TEXT("Color"))
        {
            AddField(TEXT("R"), 0.0);
            AddField(TEXT("G"), 0.0);
            AddField(TEXT("B"), 0.0);
            AddField(TEXT("A"), 1.0);
        }
        else
        {
            AddField(TEXT("X"), 0.0);
            AddField(TEXT("Y"), 0.0);
            AddField(TEXT("Z"), 0.0);
            AddField(TEXT("W"), 0.0);
        }
    }

    auto NumberAt = [&Numbers](int32 Index, double DefaultValue)
    {
        return Numbers.IsValidIndex(Index) ? Numbers[Index] : DefaultValue;
    };

    if (StructName == TEXT("LinearColor") || StructName == TEXT("Color"))
    {
        return FString::Printf(TEXT("(R=%s,G=%s,B=%s,A=%s)"),
            *FString::SanitizeFloat(NumberAt(0, 0.0)),
            *FString::SanitizeFloat(NumberAt(1, 0.0)),
            *FString::SanitizeFloat(NumberAt(2, 0.0)),
            *FString::SanitizeFloat(NumberAt(3, 1.0)));
    }
    if (StructName == TEXT("Vector2D"))
    {
        return FString::Printf(TEXT("(X=%s,Y=%s)"),
            *FString::SanitizeFloat(NumberAt(0, 0.0)),
            *FString::SanitizeFloat(NumberAt(1, 0.0)));
    }
    if (StructName == TEXT("Vector4"))
    {
        return FString::Printf(TEXT("(X=%s,Y=%s,Z=%s,W=%s)"),
            *FString::SanitizeFloat(NumberAt(0, 0.0)),
            *FString::SanitizeFloat(NumberAt(1, 0.0)),
            *FString::SanitizeFloat(NumberAt(2, 0.0)),
            *FString::SanitizeFloat(NumberAt(3, 0.0)));
    }

    return FString::Printf(TEXT("(X=%s,Y=%s,Z=%s)"),
        *FString::SanitizeFloat(NumberAt(0, 0.0)),
        *FString::SanitizeFloat(NumberAt(1, 0.0)),
        *FString::SanitizeFloat(NumberAt(2, 0.0)));
}

UObject* LoadObjectValue(const FString& ObjectPath)
{
    return LoadObject<UObject>(nullptr, *NormalizeObjectPathForLoad(ObjectPath));
}

bool IsHlslIdentifierStart(TCHAR Character)
{
    return Character == TEXT('_') ||
        (Character >= TEXT('A') && Character <= TEXT('Z')) ||
        (Character >= TEXT('a') && Character <= TEXT('z'));
}

bool IsHlslIdentifierChar(TCHAR Character)
{
    return IsHlslIdentifierStart(Character) ||
        (Character >= TEXT('0') && Character <= TEXT('9'));
}

bool IsReservedHlslIdentifier(const FString& Name)
{
    static const TSet<FString> ReservedNames = {
        TEXT("break"), TEXT("case"), TEXT("cbuffer"), TEXT("class"), TEXT("const"),
        TEXT("continue"), TEXT("default"), TEXT("discard"), TEXT("do"), TEXT("else"),
        TEXT("false"), TEXT("for"), TEXT("groupshared"), TEXT("if"), TEXT("in"),
        TEXT("inline"), TEXT("inout"), TEXT("namespace"), TEXT("out"), TEXT("return"),
        TEXT("sampler"), TEXT("samplerstate"), TEXT("static"), TEXT("struct"), TEXT("switch"),
        TEXT("texture"), TEXT("texture2d"), TEXT("true"), TEXT("typedef"), TEXT("uniform"),
        TEXT("void"), TEXT("volatile"), TEXT("while"),
        TEXT("bool"), TEXT("bool1"), TEXT("bool2"), TEXT("bool3"), TEXT("bool4"),
        TEXT("double"), TEXT("float"), TEXT("float1"), TEXT("float2"), TEXT("float3"), TEXT("float4"),
        TEXT("half"), TEXT("half1"), TEXT("half2"), TEXT("half3"), TEXT("half4"),
        TEXT("int"), TEXT("int1"), TEXT("int2"), TEXT("int3"), TEXT("int4"),
        TEXT("uint"), TEXT("uint1"), TEXT("uint2"), TEXT("uint3"), TEXT("uint4")
    };

    return ReservedNames.Contains(Name.ToLower());
}

bool IsValidHlslIdentifier(const FString& Name)
{
    if (Name.IsEmpty() || !IsHlslIdentifierStart(Name[0]))
    {
        return false;
    }

    for (int32 Index = 1; Index < Name.Len(); ++Index)
    {
        if (!IsHlslIdentifierChar(Name[Index]))
        {
            return false;
        }
    }

    return !IsReservedHlslIdentifier(Name);
}

bool ParseCustomOutputType(const FString& OutputTypeText, ECustomMaterialOutputType& OutOutputType, FString& OutError)
{
    FString Normalized = OutputTypeText.TrimStartAndEnd();
    if (Normalized.IsEmpty())
    {
        OutError = TEXT("Custom material output_type cannot be empty");
        return false;
    }

    Normalized.ReplaceInline(TEXT("_"), TEXT(""));
    Normalized.ReplaceInline(TEXT("-"), TEXT(""));
    Normalized.ReplaceInline(TEXT(" "), TEXT(""));
    Normalized = Normalized.ToUpper();

    if (Normalized.StartsWith(TEXT("CMOT")))
    {
        Normalized.RightChopInline(4);
    }

    if (Normalized == TEXT("1") || Normalized == TEXT("FLOAT") || Normalized == TEXT("FLOAT1"))
    {
        OutOutputType = CMOT_Float1;
        return true;
    }
    if (Normalized == TEXT("2") || Normalized == TEXT("FLOAT2"))
    {
        OutOutputType = CMOT_Float2;
        return true;
    }
    if (Normalized == TEXT("3") || Normalized == TEXT("FLOAT3"))
    {
        OutOutputType = CMOT_Float3;
        return true;
    }
    if (Normalized == TEXT("4") || Normalized == TEXT("FLOAT4"))
    {
        OutOutputType = CMOT_Float4;
        return true;
    }
    if (Normalized == TEXT("MATERIALATTRIBUTES") || Normalized == TEXT("ATTRIBUTES"))
    {
        OutOutputType = CMOT_MaterialAttributes;
        return true;
    }

    OutError = FString::Printf(
        TEXT("Invalid custom material output_type '%s'. Expected CMOT_Float1, CMOT_Float2, CMOT_Float3, CMOT_Float4, or CMOT_MaterialAttributes."),
        *OutputTypeText);
    return false;
}

bool TryGetStringFromJsonObjectLoose(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& FieldNames, FString& OutValue)
{
    if (!Object.IsValid())
    {
        return false;
    }

    for (const FString& FieldName : FieldNames)
    {
        if (Object->TryGetStringField(FieldName, OutValue))
        {
            return true;
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
        {
            if (Pair.Key.Equals(FieldName, ESearchCase::IgnoreCase) &&
                Pair.Value.IsValid() &&
                Pair.Value->Type == EJson::String)
            {
                OutValue = Pair.Value->AsString();
                return true;
            }
        }
    }

    return false;
}

bool ParseCustomInputNames(const TSharedPtr<FJsonObject>& Params, TArray<FName>& OutInputNames, FString& OutError)
{
    const TArray<TSharedPtr<FJsonValue>>* InputArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("inputs"), InputArray))
    {
        return true;
    }

    TSet<FString> SeenNames;
    for (int32 Index = 0; Index < InputArray->Num(); ++Index)
    {
        const TSharedPtr<FJsonValue>& InputValue = (*InputArray)[Index];
        FString InputName;
        if (InputValue.IsValid() && InputValue->Type == EJson::String)
        {
            InputName = InputValue->AsString();
        }
        else if (InputValue.IsValid() && InputValue->Type == EJson::Object)
        {
            TryGetStringFromJsonObjectLoose(InputValue->AsObject(), { TEXT("name"), TEXT("input_name") }, InputName);
        }
        else
        {
            OutError = FString::Printf(TEXT("Custom input at index %d must be a string or object with a name field"), Index);
            return false;
        }

        InputName.TrimStartAndEndInline();
        if (InputName.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Custom input at index %d has an empty name"), Index);
            return false;
        }
        if (!IsValidHlslIdentifier(InputName))
        {
            OutError = FString::Printf(TEXT("Custom input name '%s' is not a valid HLSL identifier"), *InputName);
            return false;
        }

        const FString LowerName = InputName.ToLower();
        if (SeenNames.Contains(LowerName))
        {
            OutError = FString::Printf(TEXT("Duplicate custom input name: %s"), *InputName);
            return false;
        }

        SeenNames.Add(LowerName);
        OutInputNames.Add(FName(*InputName));
    }

    return true;
}

bool ParseCustomDefines(const TSharedPtr<FJsonObject>& Params, TArray<FCustomDefine>& OutDefines, FString& OutError)
{
    const TArray<TSharedPtr<FJsonValue>>* DefineArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("additional_defines"), DefineArray))
    {
        return true;
    }

    TSet<FString> SeenNames;
    for (int32 Index = 0; Index < DefineArray->Num(); ++Index)
    {
        const TSharedPtr<FJsonValue>& DefineValue = (*DefineArray)[Index];
        if (!DefineValue.IsValid() || DefineValue->Type != EJson::Object)
        {
            OutError = FString::Printf(TEXT("additional_defines entry at index %d must be an object"), Index);
            return false;
        }

        const TSharedPtr<FJsonObject> DefineObject = DefineValue->AsObject();
        FString DefineName;
        if (!TryGetStringFromJsonObjectLoose(DefineObject, { TEXT("name"), TEXT("define_name") }, DefineName))
        {
            OutError = FString::Printf(TEXT("additional_defines entry at index %d is missing a string name"), Index);
            return false;
        }
        DefineName.TrimStartAndEndInline();
        if (!IsValidHlslIdentifier(DefineName))
        {
            OutError = FString::Printf(TEXT("Custom define name '%s' is not a valid HLSL identifier"), *DefineName);
            return false;
        }

        const FString LowerName = DefineName.ToLower();
        if (SeenNames.Contains(LowerName))
        {
            OutError = FString::Printf(TEXT("Duplicate custom define name: %s"), *DefineName);
            return false;
        }

        FString DefineText = TEXT("1");
        TSharedPtr<FJsonValue> RawDefineText = DefineObject->TryGetField(TEXT("value"));
        if (!RawDefineText.IsValid())
        {
            RawDefineText = DefineObject->TryGetField(TEXT("define_value"));
        }
        if (RawDefineText.IsValid())
        {
            if (RawDefineText->Type != EJson::String && RawDefineText->Type != EJson::Number && RawDefineText->Type != EJson::Boolean)
            {
                OutError = FString::Printf(TEXT("Custom define '%s' value must be a string, number, or boolean"), *DefineName);
                return false;
            }
            DefineText = JsonValueToImportText(RawDefineText);
        }

        FCustomDefine Define;
        Define.DefineName = DefineName;
        Define.DefineValue = DefineText;
        OutDefines.Add(Define);
        SeenNames.Add(LowerName);
    }

    return true;
}

bool ParseIncludeFilePaths(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutIncludeFilePaths, FString& OutError)
{
    const TArray<TSharedPtr<FJsonValue>>* IncludeArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("include_file_paths"), IncludeArray))
    {
        return true;
    }

    TSet<FString> SeenPaths;
    for (int32 Index = 0; Index < IncludeArray->Num(); ++Index)
    {
        const TSharedPtr<FJsonValue>& IncludeValue = (*IncludeArray)[Index];
        if (!IncludeValue.IsValid() || IncludeValue->Type != EJson::String)
        {
            OutError = FString::Printf(TEXT("include_file_paths entry at index %d must be a string"), Index);
            return false;
        }

        FString IncludePath = IncludeValue->AsString().TrimStartAndEnd();
        if (IncludePath.IsEmpty())
        {
            OutError = FString::Printf(TEXT("include_file_paths entry at index %d is empty"), Index);
            return false;
        }
        if (IncludePath.Contains(TEXT("\n")) || IncludePath.Contains(TEXT("\r")))
        {
            OutError = FString::Printf(TEXT("include_file_paths entry at index %d contains a newline"), Index);
            return false;
        }

        const FString LowerPath = IncludePath.ToLower();
        if (SeenPaths.Contains(LowerPath))
        {
            continue;
        }

        SeenPaths.Add(LowerPath);
        OutIncludeFilePaths.Add(IncludePath);
    }

    return true;
}

TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Strings)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FString& String : Strings)
    {
        Values.Add(MakeShared<FJsonValueString>(String));
    }
    return Values;
}

UClass* LoadClassValue(const FString& ClassPathOrName)
{
    const FString NormalizedPath = NormalizeObjectPathForLoad(ClassPathOrName);
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

bool SetObjectPropertyValue(UObject* Object, const FString& PropertyName, const TSharedPtr<FJsonValue>& Value, FString& OutError)
{
    if (!Object)
    {
        OutError = TEXT("Invalid object");
        return false;
    }

    FProperty* Property = FindPropertyByNameLoose(Object, PropertyName);
    if (!Property)
    {
        OutError = FString::Printf(TEXT("Property not found on %s: %s"), *Object->GetClass()->GetName(), *PropertyName);
        return false;
    }

    Object->Modify();
    void* PropertyAddr = Property->ContainerPtrToValuePtr<void>(Object);

    if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
    {
        BoolProperty->SetPropertyValue(PropertyAddr, Value.IsValid() && Value->AsBool());
        return true;
    }
    if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
    {
        if (!Value.IsValid() || Value->Type != EJson::Number)
        {
            OutError = FString::Printf(TEXT("Numeric property requires number value: %s"), *PropertyName);
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
    if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
    {
        NameProperty->SetPropertyValue(PropertyAddr, FName(*(Value.IsValid() ? Value->AsString() : FString())));
        return true;
    }
    if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
    {
        StringProperty->SetPropertyValue(PropertyAddr, Value.IsValid() ? Value->AsString() : FString());
        return true;
    }
    if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
    {
        TextProperty->SetPropertyValue(PropertyAddr, FText::FromString(Value.IsValid() ? Value->AsString() : FString()));
        return true;
    }
    if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
    {
        UEnum* Enum = EnumProperty->GetEnum();
        FNumericProperty* UnderlyingProperty = EnumProperty->GetUnderlyingProperty();
        if (!Enum || !UnderlyingProperty)
        {
            OutError = FString::Printf(TEXT("Invalid enum property: %s"), *PropertyName);
            return false;
        }

        int64 EnumValue = INDEX_NONE;
        if (Value.IsValid() && Value->Type == EJson::Number)
        {
            EnumValue = static_cast<int64>(Value->AsNumber());
        }
        else if (Value.IsValid())
        {
            EnumValue = Enum->GetValueByNameString(Value->AsString());
        }

        if (EnumValue == INDEX_NONE)
        {
            OutError = FString::Printf(TEXT("Enum value not found for %s: %s"), *PropertyName, Value.IsValid() ? *Value->AsString() : TEXT(""));
            return false;
        }
        UnderlyingProperty->SetIntPropertyValue(PropertyAddr, EnumValue);
        return true;
    }
    if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
    {
        UEnum* Enum = ByteProperty->GetIntPropertyEnum();
        if (Enum && Value.IsValid() && Value->Type == EJson::String)
        {
            const int64 EnumValue = Enum->GetValueByNameString(Value->AsString());
            if (EnumValue == INDEX_NONE)
            {
                OutError = FString::Printf(TEXT("Enum value not found for %s: %s"), *PropertyName, *Value->AsString());
                return false;
            }
            ByteProperty->SetPropertyValue(PropertyAddr, static_cast<uint8>(EnumValue));
            return true;
        }

        ByteProperty->SetPropertyValue(PropertyAddr, Value.IsValid() ? static_cast<uint8>(Value->AsNumber()) : 0);
        return true;
    }
    if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
    {
        if (!Value.IsValid() || Value->Type != EJson::String)
        {
            OutError = FString::Printf(TEXT("Object property requires object asset path string: %s"), *PropertyName);
            return false;
        }

        UObject* ObjectValue = LoadObjectValue(Value->AsString());
        if (!ObjectValue)
        {
            OutError = FString::Printf(TEXT("Object not found for property %s: %s"), *PropertyName, *Value->AsString());
            return false;
        }
        if (ObjectProperty->PropertyClass && !ObjectValue->IsA(ObjectProperty->PropertyClass))
        {
            OutError = FString::Printf(TEXT("Object %s is not a %s for property %s"),
                *ObjectValue->GetPathName(),
                *ObjectProperty->PropertyClass->GetName(),
                *PropertyName);
            return false;
        }
        ObjectProperty->SetObjectPropertyValue(PropertyAddr, ObjectValue);
        return true;
    }
    if (FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
    {
        if (!Value.IsValid() || Value->Type != EJson::String)
        {
            OutError = FString::Printf(TEXT("Class property requires class path/name string: %s"), *PropertyName);
            return false;
        }

        UClass* ClassValue = LoadClassValue(Value->AsString());
        if (!ClassValue)
        {
            OutError = FString::Printf(TEXT("Class not found for property %s: %s"), *PropertyName, *Value->AsString());
            return false;
        }
        ClassProperty->SetPropertyValue(PropertyAddr, ClassValue);
        return true;
    }

    FString ImportText = JsonValueToImportText(Value);
    if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
    {
        const FString StructName = StructProperty->Struct ? StructProperty->Struct->GetName() : FString();
        if ((Value.IsValid() && (Value->Type == EJson::Array || Value->Type == EJson::Object)) &&
            (StructName == TEXT("LinearColor") || StructName == TEXT("Color") ||
             StructName == TEXT("Vector") || StructName == TEXT("Vector2D") || StructName == TEXT("Vector4")))
        {
            ImportText = JsonValueToStructImportText(Value, StructName);
        }
    }

    if (!ImportText.IsEmpty() && Property->ImportText_Direct(*ImportText, PropertyAddr, Object, PPF_None) != nullptr)
    {
        return true;
    }

    OutError = FString::Printf(TEXT("Unsupported property type %s for property %s"), *Property->GetClass()->GetName(), *PropertyName);
    return false;
}

UClass* FindMaterialExpressionClass(const FString& ExpressionClassName)
{
    const FString NormalizedPath = NormalizeObjectPathForLoad(ExpressionClassName);
    if (UClass* Class = LoadObject<UClass>(nullptr, *NormalizedPath))
    {
        return Class->IsChildOf(UMaterialExpression::StaticClass()) ? Class : nullptr;
    }

    TArray<FString> CandidateClassNames;
    CandidateClassNames.Add(ExpressionClassName);

    FString StrippedName = ExpressionClassName;
    if (StrippedName.StartsWith(TEXT("U")) && StrippedName.Len() > 1)
    {
        StrippedName.RightChopInline(1);
    }
    CandidateClassNames.Add(StrippedName);

    if (!StrippedName.StartsWith(TEXT("MaterialExpression")))
    {
        CandidateClassNames.Add(FString::Printf(TEXT("MaterialExpression%s"), *StrippedName));
    }

    TArray<FString> CandidatePaths;
    for (const FString& CandidateName : CandidateClassNames)
    {
        CandidatePaths.Add(CandidateName);
        CandidatePaths.Add(FString::Printf(TEXT("/Script/Engine.%s"), *CandidateName));
    }

    for (const FString& CandidatePath : CandidatePaths)
    {
        if (UClass* Class = FindFirstObject<UClass>(*CandidatePath))
        {
            if (Class->IsChildOf(UMaterialExpression::StaticClass()) && !Class->HasAnyClassFlags(CLASS_Abstract))
            {
                return Class;
            }
        }

        if (UClass* Class = LoadObject<UClass>(nullptr, *CandidatePath))
        {
            if (Class->IsChildOf(UMaterialExpression::StaticClass()) && !Class->HasAnyClassFlags(CLASS_Abstract))
            {
                return Class;
            }
        }
    }

    return nullptr;
}

bool ShouldDeferImmediateMaterialExpressionUpdate(const UMaterialExpression* Expression)
{
    if (!Expression)
    {
        return true;
    }

    // External-code expressions, such as SkyAtmosphere nodes, can require editor
    // graph/compiler state that is not fully initialized immediately after creation.
    return Expression->IsA<UMaterialExpressionCustom>() ||
        Expression->IsA<UMaterialExpressionExternalCodeBase>();
}

void RefreshExpressionNodeSafe(UMaterialExpression* Expression)
{
    if (!Expression)
    {
        return;
    }

#if WITH_EDITORONLY_DATA
    // UMaterialExpression::RefreshNode dereferences GraphNode unconditionally.
    // GraphNode only exists while the material editor has this graph open, so
    // calling RefreshNode on an expression created headlessly (MCP, scripts)
    // crashes the editor with an access violation. Skip it when there is no
    // graph node; MarkChanged/PostEditChange still propagates the change.
    if (Expression->GraphNode)
    {
        Expression->RefreshNode(true);
    }
#endif
}

FString GetExpressionKey(const UMaterialExpression* Expression)
{
    if (!Expression)
    {
        return FString();
    }

    return FString::Printf(TEXT("path:%s"), *Expression->GetPathName());
}

FString GetExpressionLegacyId(const UMaterialExpression* Expression)
{
    if (!Expression)
    {
        return FString();
    }

    if (Expression->MaterialExpressionGuid.IsValid())
    {
        return Expression->MaterialExpressionGuid.ToString();
    }

    return Expression->GetName();
}

FString GetExpressionId(const UMaterialExpression* Expression)
{
    return GetExpressionKey(Expression);
}

FString GetExpressionGuidString(const UMaterialExpression* Expression)
{
    return Expression ? Expression->MaterialExpressionGuid.ToString() : FString();
}

int32 GetExpressionGuidDuplicateCount(
    const UMaterialExpression* Expression,
    TConstArrayView<TObjectPtr<UMaterialExpression>> AllExpressions)
{
    if (!Expression || !Expression->MaterialExpressionGuid.IsValid())
    {
        return 0;
    }

    int32 Count = 0;
    for (const TObjectPtr<UMaterialExpression>& OtherExpressionPtr : AllExpressions)
    {
        const UMaterialExpression* OtherExpression = OtherExpressionPtr.Get();
        if (OtherExpression && OtherExpression->MaterialExpressionGuid == Expression->MaterialExpressionGuid)
        {
            ++Count;
        }
    }
    return Count;
}

TArray<TSharedPtr<FJsonValue>> GetExpressionGuidCollisionMembers(
    const UMaterialExpression* Expression,
    TConstArrayView<TObjectPtr<UMaterialExpression>> AllExpressions)
{
    TArray<TSharedPtr<FJsonValue>> Members;
    if (!Expression || !Expression->MaterialExpressionGuid.IsValid())
    {
        return Members;
    }

    for (const TObjectPtr<UMaterialExpression>& OtherExpressionPtr : AllExpressions)
    {
        const UMaterialExpression* OtherExpression = OtherExpressionPtr.Get();
        if (!OtherExpression || OtherExpression->MaterialExpressionGuid != Expression->MaterialExpressionGuid)
        {
            continue;
        }

        TSharedPtr<FJsonObject> Member = MakeShared<FJsonObject>();
        Member->SetStringField(TEXT("node_key"), GetExpressionKey(OtherExpression));
        Member->SetStringField(TEXT("name"), OtherExpression->GetName());
        Member->SetStringField(TEXT("class"), OtherExpression->GetClass()->GetName());
        Members.Add(MakeShared<FJsonValueObject>(Member));
    }

    return Members;
}

void AddExpressionReferenceFields(TSharedPtr<FJsonObject> Object, const UMaterialExpression* Expression, const FString& Prefix)
{
    if (!Object.IsValid() || !Expression)
    {
        return;
    }

    Object->SetStringField(Prefix + TEXT("node_id"), GetExpressionId(Expression));
    Object->SetStringField(Prefix + TEXT("node_key"), GetExpressionKey(Expression));
    Object->SetStringField(Prefix + TEXT("legacy_node_id"), GetExpressionLegacyId(Expression));
    Object->SetStringField(Prefix + TEXT("guid"), GetExpressionGuidString(Expression));
    Object->SetStringField(Prefix + TEXT("node_name"), Expression->GetName());
    Object->SetStringField(Prefix + TEXT("object_path"), Expression->GetPathName());
}

FString GetFallbackOutputName(const FExpressionOutput& Output)
{
    if (!Output.OutputName.IsNone())
    {
        return Output.OutputName.ToString();
    }
    if (Output.MaskR && !Output.MaskG && !Output.MaskB && !Output.MaskA)
    {
        return TEXT("R");
    }
    if (!Output.MaskR && Output.MaskG && !Output.MaskB && !Output.MaskA)
    {
        return TEXT("G");
    }
    if (!Output.MaskR && !Output.MaskG && Output.MaskB && !Output.MaskA)
    {
        return TEXT("B");
    }
    if (!Output.MaskR && !Output.MaskG && !Output.MaskB && Output.MaskA)
    {
        return TEXT("A");
    }
    return FString();
}

TArray<FString> GetMaterialExpressionInputNamesSafe(UMaterialExpression* Expression)
{
    TArray<FString> InputNames;
    if (!Expression)
    {
        return InputNames;
    }

    InputNames = UMaterialEditingLibrary::GetMaterialExpressionInputNames(Expression);
    if (InputNames.Num() == 0)
    {
        for (FExpressionInputIterator It{ Expression }; It; ++It)
        {
            InputNames.Add(Expression->GetInputName(It.Index).ToString());
        }
    }

    return InputNames;
}

TArray<FString> GetMaterialExpressionOutputNames(UMaterialExpression* Expression)
{
    TArray<FString> OutputNames;
    if (!Expression)
    {
        return OutputNames;
    }

    for (const FExpressionOutput& Output : Expression->Outputs)
    {
        OutputNames.Add(GetFallbackOutputName(Output));
    }

    return OutputNames;
}

int32 FindOutputIndexByName(UMaterialExpression* Expression, const FString& OutputName)
{
    if (!Expression || Expression->Outputs.Num() == 0)
    {
        return INDEX_NONE;
    }
    if (OutputName.IsEmpty())
    {
        return 0;
    }

    for (int32 Index = 0; Index < Expression->Outputs.Num(); ++Index)
    {
        const FString CandidateName = GetFallbackOutputName(Expression->Outputs[Index]);
        if (CandidateName.Equals(OutputName, ESearchCase::IgnoreCase))
        {
            return Index;
        }
    }

    return INDEX_NONE;
}

FString ResolveOutputConnectName(UMaterialExpression* Expression, const FString& RequestedOutputName)
{
    if (!Expression || RequestedOutputName.IsEmpty())
    {
        return FString();
    }

    for (const FString& CandidateName : GetMaterialExpressionOutputNames(Expression))
    {
        if (CandidateName.Equals(RequestedOutputName, ESearchCase::IgnoreCase))
        {
            return CandidateName;
        }
    }

    return RequestedOutputName;
}

bool HasInputName(UMaterialExpression* Expression, const FString& InputName)
{
    if (!Expression)
    {
        return false;
    }
    if (InputName.IsEmpty())
    {
        return Expression->GetInput(0) != nullptr;
    }

    const TArray<FString> InputNames = GetMaterialExpressionInputNamesSafe(Expression);
    for (const FString& CandidateName : InputNames)
    {
        if (CandidateName.Equals(InputName, ESearchCase::IgnoreCase))
        {
            return true;
        }
    }

    return false;
}

FString ResolveInputConnectName(UMaterialExpression* Expression, const FString& RequestedInputName)
{
    if (!Expression || RequestedInputName.IsEmpty())
    {
        return FString();
    }

    for (const FString& CandidateName : GetMaterialExpressionInputNamesSafe(Expression))
    {
        if (CandidateName.Equals(RequestedInputName, ESearchCase::IgnoreCase))
        {
            return CandidateName;
        }
    }

    return RequestedInputName;
}

bool FindExpressionById(const FMaterialGraphTarget& Target, const FString& NodeId, UMaterialExpression*& OutExpression, FString& OutError)
{
    TArray<UMaterialExpression*> Matches;
    FString Query = NodeId.TrimStartAndEnd();
    FString Prefix;
    FString Value = Query;
    if (Query.Split(TEXT(":"), &Prefix, &Value))
    {
        Prefix = Prefix.ToLower();
    }
    else
    {
        Prefix.Reset();
    }

    for (const TObjectPtr<UMaterialExpression>& ExpressionPtr : Target.GetExpressions())
    {
        UMaterialExpression* Expression = ExpressionPtr.Get();
        if (!Expression)
        {
            continue;
        }

        bool bMatches = false;
        if (Prefix == TEXT("path"))
        {
            bMatches = Expression->GetPathName().Equals(Value, ESearchCase::IgnoreCase);
        }
        else if (Prefix == TEXT("name"))
        {
            bMatches = Expression->GetName().Equals(Value, ESearchCase::IgnoreCase);
        }
        else if (Prefix == TEXT("guid"))
        {
            bMatches = GetExpressionGuidString(Expression).Equals(Value, ESearchCase::IgnoreCase);
        }
        else
        {
            bMatches =
                GetExpressionId(Expression).Equals(Query, ESearchCase::IgnoreCase) ||
                GetExpressionKey(Expression).Equals(Query, ESearchCase::IgnoreCase) ||
                GetExpressionLegacyId(Expression).Equals(Query, ESearchCase::IgnoreCase) ||
                GetExpressionGuidString(Expression).Equals(Query, ESearchCase::IgnoreCase) ||
                Expression->GetName().Equals(Query, ESearchCase::IgnoreCase) ||
                Expression->GetPathName().Equals(Query, ESearchCase::IgnoreCase);
        }

        if (bMatches)
        {
            Matches.Add(Expression);
        }
    }

    if (Matches.Num() == 1)
    {
        OutExpression = Matches[0];
        return true;
    }
    if (Matches.Num() > 1)
    {
        TArray<FString> MatchIds;
        for (UMaterialExpression* Match : Matches)
        {
            MatchIds.Add(FString::Printf(TEXT("%s guid=%s (%s)"), *GetExpressionKey(Match), *GetExpressionGuidString(Match), *Match->GetClass()->GetName()));
        }
        OutError = FString::Printf(TEXT("Ambiguous material node id '%s'. Matches: %s"), *NodeId, *FString::Join(MatchIds, TEXT(", ")));
        return false;
    }

    OutError = FString::Printf(TEXT("Material node not found: %s"), *NodeId);
    return false;
}

TSharedPtr<FJsonObject> ExpressionInputToJson(UMaterialExpression* Expression, int32 InputIndex, const FString& InputName)
{
    TSharedPtr<FJsonObject> InputObject = MakeShared<FJsonObject>();
    InputObject->SetStringField(TEXT("name"), InputName);
    InputObject->SetNumberField(TEXT("index"), InputIndex);

    if (!Expression)
    {
        return InputObject;
    }

    InputObject->SetStringField(TEXT("raw_name"), Expression->GetInputName(InputIndex).ToString());
    InputObject->SetBoolField(TEXT("required"), Expression->IsInputConnectionRequired(InputIndex));
    InputObject->SetNumberField(TEXT("value_type"), static_cast<int32>(Expression->GetInputValueType(InputIndex)));

    FExpressionInput* Input = Expression->GetInput(InputIndex);
    if (Input && Input->Expression)
    {
        InputObject->SetBoolField(TEXT("connected"), true);
        AddExpressionReferenceFields(InputObject, Input->Expression, TEXT("connected_"));
        InputObject->SetStringField(TEXT("connected_output"), Input->Expression->Outputs.IsValidIndex(Input->OutputIndex)
            ? GetFallbackOutputName(Input->Expression->Outputs[Input->OutputIndex])
            : FString());
        InputObject->SetNumberField(TEXT("connected_output_index"), Input->OutputIndex);
    }
    else
    {
        InputObject->SetBoolField(TEXT("connected"), false);
    }

    return InputObject;
}

TSharedPtr<FJsonObject> ExpressionOutputToJson(
    UMaterialExpression* Expression,
    int32 OutputIndex,
    const FExpressionOutput& Output,
    TConstArrayView<TObjectPtr<UMaterialExpression>> AllExpressions)
{
    TSharedPtr<FJsonObject> OutputObject = MakeShared<FJsonObject>();
    const FString OutputName = GetFallbackOutputName(Output);
    OutputObject->SetStringField(TEXT("name"), OutputName);
    OutputObject->SetStringField(TEXT("connect_name"), OutputName);
    OutputObject->SetNumberField(TEXT("index"), OutputIndex);
    OutputObject->SetNumberField(TEXT("value_type"), Expression ? static_cast<int32>(Expression->GetOutputValueType(OutputIndex)) : 0);
    OutputObject->SetBoolField(TEXT("mask"), Output.Mask != 0);
    OutputObject->SetBoolField(TEXT("mask_r"), Output.MaskR != 0);
    OutputObject->SetBoolField(TEXT("mask_g"), Output.MaskG != 0);
    OutputObject->SetBoolField(TEXT("mask_b"), Output.MaskB != 0);
    OutputObject->SetBoolField(TEXT("mask_a"), Output.MaskA != 0);

    TArray<TSharedPtr<FJsonValue>> ConnectedTo;
    for (const TObjectPtr<UMaterialExpression>& OtherExpressionPtr : AllExpressions)
    {
        UMaterialExpression* OtherExpression = OtherExpressionPtr.Get();
        if (!OtherExpression)
        {
            continue;
        }

        const TArray<FString> InputNames = GetMaterialExpressionInputNamesSafe(OtherExpression);
        for (int32 InputIndex = 0; InputIndex < InputNames.Num(); ++InputIndex)
        {
            FExpressionInput* Input = OtherExpression->GetInput(InputIndex);
            if (Input && Input->Expression == Expression && Input->OutputIndex == OutputIndex)
            {
                TSharedPtr<FJsonObject> ConnectionObject = MakeShared<FJsonObject>();
                AddExpressionReferenceFields(ConnectionObject, OtherExpression, TEXT(""));
                ConnectionObject->SetStringField(TEXT("input"), InputNames[InputIndex]);
                ConnectionObject->SetNumberField(TEXT("input_index"), InputIndex);
                ConnectedTo.Add(MakeShared<FJsonValueObject>(ConnectionObject));
            }
        }
    }
    OutputObject->SetArrayField(TEXT("connected_to"), ConnectedTo);

    return OutputObject;
}

FString ObjectPathOrEmpty(const UObject* Object)
{
    return Object ? Object->GetPathName() : FString();
}

FString EnumNameOrValue(UEnum* Enum, int64 Value)
{
    if (!Enum)
    {
        return FString::FromInt(static_cast<int32>(Value));
    }

    FString Name = Enum->GetNameStringByValue(Value);
    return Name.IsEmpty() ? FString::FromInt(static_cast<int32>(Value)) : Name;
}

TSharedPtr<FJsonObject> FunctionCallToJson(UMaterialExpressionMaterialFunctionCall* FunctionCall)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    if (!FunctionCall)
    {
        return Object;
    }

    UMaterialFunctionInterface* Function = FunctionCall->MaterialFunction;
    Object->SetBoolField(TEXT("has_function"), Function != nullptr);
    Object->SetStringField(TEXT("function_path"), ObjectPathOrEmpty(Function));
    Object->SetStringField(TEXT("function_name"), Function ? Function->GetName() : FString());
    Object->SetStringField(TEXT("function_class"), Function ? Function->GetClass()->GetName() : FString());

    UEnum* InputTypeEnum = StaticEnum<EFunctionInputType>();
    TArray<TSharedPtr<FJsonValue>> Inputs;
    for (int32 Index = 0; Index < FunctionCall->FunctionInputs.Num(); ++Index)
    {
        const FFunctionExpressionInput& FunctionInput = FunctionCall->FunctionInputs[Index];
        TSharedPtr<FJsonObject> InputObject = MakeShared<FJsonObject>();
        InputObject->SetNumberField(TEXT("index"), Index);
        InputObject->SetStringField(TEXT("id"), FunctionInput.ExpressionInputId.ToString());
        if (UMaterialExpressionFunctionInput* ExpressionInput = FunctionInput.ExpressionInput)
        {
            InputObject->SetStringField(TEXT("name"), ExpressionInput->InputName.ToString());
            InputObject->SetStringField(TEXT("description"), ExpressionInput->Description);
            InputObject->SetStringField(TEXT("type"), EnumNameOrValue(InputTypeEnum, static_cast<int64>(ExpressionInput->InputType.GetValue())));
            InputObject->SetNumberField(TEXT("sort_priority"), ExpressionInput->SortPriority);
            InputObject->SetBoolField(TEXT("use_preview_as_default"), ExpressionInput->bUsePreviewValueAsDefault != 0);
        }
        InputObject->SetBoolField(TEXT("connected"), FunctionInput.Input.Expression != nullptr);
        if (FunctionInput.Input.Expression)
        {
            AddExpressionReferenceFields(InputObject, FunctionInput.Input.Expression, TEXT("connected_"));
            InputObject->SetNumberField(TEXT("connected_output_index"), FunctionInput.Input.OutputIndex);
        }
        Inputs.Add(MakeShared<FJsonValueObject>(InputObject));
    }
    Object->SetArrayField(TEXT("inputs"), Inputs);

    TArray<TSharedPtr<FJsonValue>> Outputs;
    for (int32 Index = 0; Index < FunctionCall->FunctionOutputs.Num(); ++Index)
    {
        const FFunctionExpressionOutput& FunctionOutput = FunctionCall->FunctionOutputs[Index];
        TSharedPtr<FJsonObject> OutputObject = MakeShared<FJsonObject>();
        OutputObject->SetNumberField(TEXT("index"), Index);
        OutputObject->SetStringField(TEXT("id"), FunctionOutput.ExpressionOutputId.ToString());
        OutputObject->SetStringField(TEXT("connect_name"), GetFallbackOutputName(FunctionOutput.Output));
        if (UMaterialExpressionFunctionOutput* ExpressionOutput = FunctionOutput.ExpressionOutput)
        {
            OutputObject->SetStringField(TEXT("name"), ExpressionOutput->OutputName.ToString());
            OutputObject->SetStringField(TEXT("description"), ExpressionOutput->Description);
            OutputObject->SetNumberField(TEXT("sort_priority"), ExpressionOutput->SortPriority);
        }
        Outputs.Add(MakeShared<FJsonValueObject>(OutputObject));
    }
    Object->SetArrayField(TEXT("outputs"), Outputs);

    return Object;
}

TSharedPtr<FJsonObject> RerouteToJson(
    UMaterialExpressionRerouteBase* Reroute,
    TConstArrayView<TObjectPtr<UMaterialExpression>> AllExpressions)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    if (!Reroute)
    {
        return Object;
    }

    int32 OutputIndex = INDEX_NONE;
    UMaterialExpression* TracedExpression = Reroute->TraceInputsToRealExpression(OutputIndex);
    Object->SetBoolField(TEXT("trace_resolved"), TracedExpression != nullptr);
    Object->SetNumberField(TEXT("trace_output_index"), OutputIndex);
    if (TracedExpression)
    {
        AddExpressionReferenceFields(Object, TracedExpression, TEXT("trace_source_"));
        Object->SetStringField(TEXT("trace_output"), TracedExpression->Outputs.IsValidIndex(OutputIndex)
            ? GetFallbackOutputName(TracedExpression->Outputs[OutputIndex])
            : FString());
    }

    if (UMaterialExpressionNamedRerouteDeclaration* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(Reroute))
    {
        Object->SetStringField(TEXT("kind"), TEXT("named_declaration"));
        Object->SetStringField(TEXT("name"), Declaration->Name.ToString());
        Object->SetStringField(TEXT("variable_guid"), Declaration->VariableGuid.ToString());
        Object->SetStringField(TEXT("node_color"), Declaration->NodeColor.ToString());

        TArray<TSharedPtr<FJsonValue>> Usages;
        for (const TObjectPtr<UMaterialExpression>& ExpressionPtr : AllExpressions)
        {
            UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(ExpressionPtr.Get());
            if (!Usage)
            {
                continue;
            }
            if (Usage->Declaration == Declaration || Usage->DeclarationGuid == Declaration->VariableGuid)
            {
                TSharedPtr<FJsonObject> UsageObject = MakeShared<FJsonObject>();
                AddExpressionReferenceFields(UsageObject, Usage, TEXT(""));
                UsageObject->SetStringField(TEXT("declaration_guid"), Usage->DeclarationGuid.ToString());
                UsageObject->SetBoolField(TEXT("declaration_valid"), Usage->Declaration != nullptr);
                Usages.Add(MakeShared<FJsonValueObject>(UsageObject));
            }
        }
        Object->SetArrayField(TEXT("usages"), Usages);
        Object->SetNumberField(TEXT("usage_count"), Usages.Num());
    }
    else if (UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(Reroute))
    {
        Object->SetStringField(TEXT("kind"), TEXT("named_usage"));
        Object->SetStringField(TEXT("declaration_guid"), Usage->DeclarationGuid.ToString());
        Object->SetBoolField(TEXT("declaration_valid"), Usage->Declaration != nullptr);
        if (Usage->Declaration)
        {
            AddExpressionReferenceFields(Object, Usage->Declaration, TEXT("declaration_"));
            Object->SetStringField(TEXT("declaration_name"), Usage->Declaration->Name.ToString());
            Object->SetStringField(TEXT("variable_guid"), Usage->Declaration->VariableGuid.ToString());
        }
    }
    else
    {
        Object->SetStringField(TEXT("kind"), TEXT("reroute"));
    }

    return Object;
}

FString ExportStringProperty(UObject* Object, const TCHAR* PropertyName)
{
    if (!Object)
    {
        return FString();
    }

    FProperty* Property = FindPropertyByNameLoose(Object, PropertyName);
    if (!Property)
    {
        return FString();
    }

    FString Exported;
    Property->ExportTextItem_Direct(Exported, Property->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
    Exported.TrimQuotesInline();
    return Exported;
}

TSharedPtr<FJsonObject> CustomHlslToJson(UMaterialExpression* Expression)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    if (!Expression)
    {
        return Object;
    }

    const FString Code = ExportStringProperty(Expression, TEXT("Code"));
    Object->SetBoolField(TEXT("has_code"), !Code.IsEmpty());
    Object->SetNumberField(TEXT("code_length"), Code.Len());
    Object->SetStringField(TEXT("code_preview"), Code.Left(512));
    Object->SetStringField(TEXT("output_type"), ExportStringProperty(Expression, TEXT("OutputType")));
    Object->SetStringField(TEXT("include_file_paths"), ExportStringProperty(Expression, TEXT("IncludeFilePaths")));
    Object->SetStringField(TEXT("additional_defines"), ExportStringProperty(Expression, TEXT("AdditionalDefines")));
    return Object;
}

TSharedPtr<FJsonObject> StaticSwitchToJson(UMaterialExpression* Expression)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    if (!Expression)
    {
        return Object;
    }

    if (UMaterialExpressionParameter* Parameter = Cast<UMaterialExpressionParameter>(Expression))
    {
        Object->SetStringField(TEXT("parameter_name"), Parameter->ParameterName.ToString());
        Object->SetStringField(TEXT("parameter_guid"), Parameter->ExpressionGUID.ToString());
        Object->SetStringField(TEXT("group"), Parameter->Group.ToString());
        Object->SetNumberField(TEXT("sort_priority"), Parameter->SortPriority);
    }
    if (UMaterialExpressionStaticBoolParameter* StaticBool = Cast<UMaterialExpressionStaticBoolParameter>(Expression))
    {
        Object->SetBoolField(TEXT("default_value"), StaticBool->DefaultValue != 0);
        Object->SetBoolField(TEXT("dynamic_branch"), StaticBool->DynamicBranch != 0);
    }
    return Object;
}

TSharedPtr<FJsonObject> ExpressionToJson(
    UMaterialExpression* Expression,
    bool bIncludePins,
    TConstArrayView<TObjectPtr<UMaterialExpression>> AllExpressions)
{
    TSharedPtr<FJsonObject> NodeObject = MakeShared<FJsonObject>();
    if (!Expression)
    {
        return NodeObject;
    }

    const int32 GuidDuplicateCount = GetExpressionGuidDuplicateCount(Expression, AllExpressions);
    NodeObject->SetStringField(TEXT("node_id"), GetExpressionId(Expression));
    NodeObject->SetStringField(TEXT("node_key"), GetExpressionKey(Expression));
    NodeObject->SetStringField(TEXT("legacy_node_id"), GetExpressionLegacyId(Expression));
    NodeObject->SetStringField(TEXT("guid"), GetExpressionGuidString(Expression));
    NodeObject->SetBoolField(TEXT("guid_valid"), Expression->MaterialExpressionGuid.IsValid());
    NodeObject->SetBoolField(TEXT("guid_duplicate"), GuidDuplicateCount > 1);
    NodeObject->SetNumberField(TEXT("guid_duplicate_count"), GuidDuplicateCount);
    if (GuidDuplicateCount > 1)
    {
        NodeObject->SetArrayField(TEXT("guid_collision_members"), GetExpressionGuidCollisionMembers(Expression, AllExpressions));
    }
    NodeObject->SetStringField(TEXT("name"), Expression->GetName());
    NodeObject->SetStringField(TEXT("object_path"), Expression->GetPathName());
    NodeObject->SetStringField(TEXT("class"), Expression->GetClass()->GetName());
    NodeObject->SetStringField(TEXT("class_path"), Expression->GetClass()->GetPathName());
    NodeObject->SetStringField(TEXT("desc"), Expression->Desc);
    NodeObject->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
    NodeObject->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);

    if (bIncludePins)
    {
        TArray<TSharedPtr<FJsonValue>> InputArray;
        const TArray<FString> InputNames = GetMaterialExpressionInputNamesSafe(Expression);
        for (int32 InputIndex = 0; InputIndex < InputNames.Num(); ++InputIndex)
        {
            InputArray.Add(MakeShared<FJsonValueObject>(ExpressionInputToJson(Expression, InputIndex, InputNames[InputIndex])));
        }
        NodeObject->SetArrayField(TEXT("inputs"), InputArray);

        TArray<TSharedPtr<FJsonValue>> OutputArray;
        for (int32 OutputIndex = 0; OutputIndex < Expression->Outputs.Num(); ++OutputIndex)
        {
            OutputArray.Add(MakeShared<FJsonValueObject>(ExpressionOutputToJson(Expression, OutputIndex, Expression->Outputs[OutputIndex], AllExpressions)));
        }
        NodeObject->SetArrayField(TEXT("outputs"), OutputArray);
    }

    if (UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
    {
        NodeObject->SetObjectField(TEXT("function_call"), FunctionCallToJson(FunctionCall));
    }
    if (UMaterialExpressionRerouteBase* Reroute = Cast<UMaterialExpressionRerouteBase>(Expression))
    {
        NodeObject->SetObjectField(TEXT("reroute"), RerouteToJson(Reroute, AllExpressions));
    }
    if (Expression->GetClass()->GetName().Contains(TEXT("MaterialExpressionCustom")))
    {
        NodeObject->SetObjectField(TEXT("custom_hlsl"), CustomHlslToJson(Expression));
    }
    if (Cast<UMaterialExpressionStaticBoolParameter>(Expression) ||
        Expression->GetClass()->GetName().Contains(TEXT("StaticSwitch")))
    {
        NodeObject->SetObjectField(TEXT("static_switch"), StaticSwitchToJson(Expression));
    }

    return NodeObject;
}

TSharedPtr<FJsonObject> MaterialPropertyConnectionToJson(UMaterial* Material, EMaterialProperty Property)
{
    TSharedPtr<FJsonObject> PropertyObject = MakeShared<FJsonObject>();
    UEnum* MaterialPropertyEnum = StaticEnum<EMaterialProperty>();
    const FString PropertyName = MaterialPropertyEnum ? MaterialPropertyEnum->GetNameStringByValue(static_cast<int64>(Property)) : FString::FromInt(static_cast<int32>(Property));
    PropertyObject->SetStringField(TEXT("property"), PropertyName);
    PropertyObject->SetNumberField(TEXT("property_value"), static_cast<int32>(Property));
    if (MaterialPropertyEnum)
    {
        PropertyObject->SetStringField(TEXT("display_name"), MaterialPropertyEnum->GetDisplayNameTextByValue(static_cast<int64>(Property)).ToString());
    }

    FExpressionInput* Input = Material ? Material->GetExpressionInputForProperty(Property) : nullptr;
    if (Input && Input->Expression)
    {
        PropertyObject->SetBoolField(TEXT("connected"), true);
        AddExpressionReferenceFields(PropertyObject, Input->Expression, TEXT(""));
        PropertyObject->SetStringField(TEXT("output"), Input->Expression->Outputs.IsValidIndex(Input->OutputIndex)
            ? GetFallbackOutputName(Input->Expression->Outputs[Input->OutputIndex])
            : FString());
        PropertyObject->SetNumberField(TEXT("output_index"), Input->OutputIndex);
    }
    else
    {
        PropertyObject->SetBoolField(TEXT("connected"), false);
    }

    return PropertyObject;
}

bool MaterialPropertyFromString(const FString& PropertyName, EMaterialProperty& OutProperty, FString& OutError)
{
    UEnum* MaterialPropertyEnum = StaticEnum<EMaterialProperty>();
    if (MaterialPropertyEnum)
    {
        int64 EnumValue = MaterialPropertyEnum->GetValueByNameString(PropertyName);
        if (EnumValue == INDEX_NONE && !PropertyName.StartsWith(TEXT("MP_")))
        {
            EnumValue = MaterialPropertyEnum->GetValueByNameString(FString::Printf(TEXT("MP_%s"), *PropertyName));
        }
        if (EnumValue != INDEX_NONE && EnumValue < MP_MAX)
        {
            OutProperty = static_cast<EMaterialProperty>(EnumValue);
            return true;
        }
    }

    FString Normalized = PropertyName.ToLower();
    Normalized.ReplaceInline(TEXT("mp_"), TEXT(""));
    Normalized.ReplaceInline(TEXT("_"), TEXT(""));
    Normalized.ReplaceInline(TEXT(" "), TEXT(""));
    Normalized.ReplaceInline(TEXT("-"), TEXT(""));

    static const TMap<FString, EMaterialProperty> CommonNames =
    {
        { TEXT("emissive"), MP_EmissiveColor },
        { TEXT("emissivecolor"), MP_EmissiveColor },
        { TEXT("opacity"), MP_Opacity },
        { TEXT("opacitymask"), MP_OpacityMask },
        { TEXT("basecolor"), MP_BaseColor },
        { TEXT("diffuse"), MP_BaseColor },
        { TEXT("albedo"), MP_BaseColor },
        { TEXT("metallic"), MP_Metallic },
        { TEXT("specular"), MP_Specular },
        { TEXT("roughness"), MP_Roughness },
        { TEXT("anisotropy"), MP_Anisotropy },
        { TEXT("normal"), MP_Normal },
        { TEXT("tangent"), MP_Tangent },
        { TEXT("worldpositionoffset"), MP_WorldPositionOffset },
        { TEXT("wpo"), MP_WorldPositionOffset },
        { TEXT("subsurface"), MP_SubsurfaceColor },
        { TEXT("subsurfacecolor"), MP_SubsurfaceColor },
        { TEXT("clearcoat"), MP_CustomData0 },
        { TEXT("clearcoatamount"), MP_CustomData0 },
        { TEXT("clearcoatroughness"), MP_CustomData1 },
        { TEXT("customdata0"), MP_CustomData0 },
        { TEXT("customdata1"), MP_CustomData1 },
        { TEXT("ambientocclusion"), MP_AmbientOcclusion },
        { TEXT("ao"), MP_AmbientOcclusion },
        { TEXT("refraction"), MP_Refraction },
        { TEXT("pixeldepthoffset"), MP_PixelDepthOffset },
        { TEXT("pdo"), MP_PixelDepthOffset },
        { TEXT("frontmaterial"), MP_FrontMaterial },
        { TEXT("surfacethickness"), MP_SurfaceThickness },
        { TEXT("displacement"), MP_Displacement },
        { TEXT("materialattributes"), MP_MaterialAttributes },
    };

    if (const EMaterialProperty* FoundProperty = CommonNames.Find(Normalized))
    {
        OutProperty = *FoundProperty;
        return true;
    }

    OutError = FString::Printf(TEXT("Unknown material property '%s'. Use names like BaseColor, Roughness, Normal, or MP_BaseColor."), *PropertyName);
    return false;
}

TSharedPtr<FJsonObject> LinearColorToJson(const FLinearColor& Color)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetNumberField(TEXT("r"), Color.R);
    Object->SetNumberField(TEXT("g"), Color.G);
    Object->SetNumberField(TEXT("b"), Color.B);
    Object->SetNumberField(TEXT("a"), Color.A);
    return Object;
}

TSharedPtr<FJsonObject> Vector4dToJson(const FVector4d& Value)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetNumberField(TEXT("x"), Value.X);
    Object->SetNumberField(TEXT("y"), Value.Y);
    Object->SetNumberField(TEXT("z"), Value.Z);
    Object->SetNumberField(TEXT("w"), Value.W);
    return Object;
}

TSharedPtr<FJsonObject> ParameterInfoToJson(const FMaterialParameterInfo& Info)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    UEnum* AssociationEnum = StaticEnum<EMaterialParameterAssociation>();
    Object->SetStringField(TEXT("name"), Info.Name.ToString());
    Object->SetStringField(TEXT("association"), EnumNameOrValue(AssociationEnum, static_cast<int64>(Info.Association.GetValue())));
    Object->SetNumberField(TEXT("index"), Info.Index);
    Object->SetStringField(TEXT("key"), Info.ToString());
    return Object;
}

TArray<TSharedPtr<FJsonValue>> ShadingModelsToJson(FMaterialShadingModelField ShadingModels)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    UEnum* ShadingModelEnum = StaticEnum<EMaterialShadingModel>();
    for (int32 Index = 0; Index < MSM_NUM; ++Index)
    {
        const EMaterialShadingModel ShadingModel = static_cast<EMaterialShadingModel>(Index);
        if (!ShadingModels.HasShadingModel(ShadingModel))
        {
            continue;
        }

        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("name"), EnumNameOrValue(ShadingModelEnum, Index));
        Object->SetNumberField(TEXT("value"), Index);
        Values.Add(MakeShared<FJsonValueObject>(Object));
    }
    return Values;
}

TSharedPtr<FJsonObject> MaterialSettingsToJson(UMaterialInterface* MaterialInterface, const UMaterial* BaseMaterial)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    if (!MaterialInterface && !BaseMaterial)
    {
        return Object;
    }

    UEnum* DomainEnum = StaticEnum<EMaterialDomain>();
    UEnum* BlendEnum = StaticEnum<EBlendMode>();
    const UMaterial* EffectiveBase = BaseMaterial ? BaseMaterial : (MaterialInterface ? MaterialInterface->GetMaterial() : nullptr);
    const FMaterialShadingModelField ShadingModels = MaterialInterface ? MaterialInterface->GetShadingModels() : (EffectiveBase ? EffectiveBase->GetShadingModels() : FMaterialShadingModelField());
    const EBlendMode BlendMode = MaterialInterface ? MaterialInterface->GetBlendMode() : (EffectiveBase ? EffectiveBase->BlendMode.GetValue() : BLEND_Opaque);
    const EMaterialDomain Domain = EffectiveBase ? EffectiveBase->MaterialDomain.GetValue() : MD_Surface;

    Object->SetStringField(TEXT("domain"), EnumNameOrValue(DomainEnum, static_cast<int64>(Domain)));
    Object->SetStringField(TEXT("domain_display"), MaterialDomainString(Domain));
    Object->SetStringField(TEXT("blend_mode"), EnumNameOrValue(BlendEnum, static_cast<int64>(BlendMode)));
    Object->SetNumberField(TEXT("blend_mode_value"), static_cast<int32>(BlendMode));
    Object->SetArrayField(TEXT("shading_models"), ShadingModelsToJson(ShadingModels));
    Object->SetNumberField(TEXT("shading_model_field"), ShadingModels.GetShadingModelField());
    Object->SetBoolField(TEXT("two_sided"), MaterialInterface ? MaterialInterface->IsTwoSided() : (EffectiveBase ? EffectiveBase->TwoSided != 0 : false));
    Object->SetBoolField(TEXT("use_material_attributes"), EffectiveBase ? EffectiveBase->bUseMaterialAttributes != 0 : false);
    Object->SetBoolField(TEXT("thin_surface"), EffectiveBase ? EffectiveBase->bIsThinSurface != 0 : false);
    Object->SetBoolField(TEXT("substrate_detected"), ShadingModels.HasShadingModel(MSM_Strata));
    Object->SetStringField(TEXT("base_material"), ObjectPathOrEmpty(EffectiveBase));
    return Object;
}

bool HasShadingModel(FMaterialShadingModelField ShadingModels, EMaterialShadingModel ShadingModel)
{
    return ShadingModels.HasShadingModel(ShadingModel);
}

void ResolveMaterialPropertySemantic(
    EMaterialProperty Property,
    EMaterialDomain Domain,
    EBlendMode BlendMode,
    FMaterialShadingModelField ShadingModels,
    FString& OutSemanticName,
    FString& OutSemanticRole,
    FString& OutShaderHint)
{
    UEnum* MaterialPropertyEnum = StaticEnum<EMaterialProperty>();
    OutSemanticName = MaterialPropertyEnum ? MaterialPropertyEnum->GetNameStringByValue(static_cast<int64>(Property)) : FString::FromInt(static_cast<int32>(Property));
    OutSemanticRole = TEXT("standard_material_property");
    OutShaderHint.Reset();

    if (Domain == MD_LightFunction)
    {
        if (Property == MP_EmissiveColor)
        {
            OutSemanticName = TEXT("LightFunctionIntensity");
            OutSemanticRole = TEXT("light_function_output");
        }
        return;
    }
    if (Domain == MD_PostProcess)
    {
        if (Property == MP_EmissiveColor)
        {
            OutSemanticName = TEXT("PostProcessColor");
            OutSemanticRole = TEXT("post_process_output");
        }
        else if (Property == MP_Opacity)
        {
            OutSemanticName = TEXT("PostProcessOpacity");
            OutSemanticRole = TEXT("post_process_blend_weight");
        }
        return;
    }
    if (Domain == MD_Volume)
    {
        if (Property == MP_SubsurfaceColor)
        {
            OutSemanticName = TEXT("VolumeScatteringOrExtinction");
            OutSemanticRole = TEXT("volume_material_input");
            OutShaderHint = TEXT("Volume domain uses a reduced property set; do not interpret this as surface subsurface shading.");
        }
        return;
    }
    if (Domain == MD_UI)
    {
        if (Property == MP_EmissiveColor)
        {
            OutSemanticName = TEXT("UIFinalColor");
            OutSemanticRole = TEXT("ui_color_output");
        }
        return;
    }
    if (Domain == MD_DeferredDecal)
    {
        OutSemanticRole = TEXT("deferred_decal_property");
        if (Property == MP_Opacity)
        {
            OutSemanticName = TEXT("DecalOpacity");
        }
        return;
    }

    if (Property == MP_CustomData0)
    {
        if (HasShadingModel(ShadingModels, MSM_ClearCoat))
        {
            OutSemanticName = TEXT("ClearCoat");
            OutSemanticRole = TEXT("clear_coat_lobe_weight");
            OutShaderHint = TEXT("ClearCoatCommon.ush / ShadingModels.ush read this through GBuffer.CustomData.x.");
        }
        else if (HasShadingModel(ShadingModels, MSM_Hair))
        {
            OutSemanticName = TEXT("HairBacklit");
            OutSemanticRole = TEXT("hair_custom_data");
        }
        else if (HasShadingModel(ShadingModels, MSM_Cloth))
        {
            OutSemanticName = TEXT("ClothAmount");
            OutSemanticRole = TEXT("cloth_custom_data");
        }
        else if (HasShadingModel(ShadingModels, MSM_Eye))
        {
            OutSemanticName = TEXT("IrisMask");
            OutSemanticRole = TEXT("eye_custom_data");
        }
        else if (HasShadingModel(ShadingModels, MSM_SubsurfaceProfile))
        {
            OutSemanticName = TEXT("SubsurfaceProfileOpacity");
            OutSemanticRole = TEXT("subsurface_profile_custom_data");
        }
        else
        {
            OutSemanticRole = TEXT("shading_model_custom_data");
        }
    }
    else if (Property == MP_CustomData1)
    {
        if (HasShadingModel(ShadingModels, MSM_ClearCoat))
        {
            OutSemanticName = TEXT("ClearCoatRoughness");
            OutSemanticRole = TEXT("clear_coat_lobe_roughness");
            OutShaderHint = TEXT("ClearCoatCommon.ush / ShadingModels.ush read this through GBuffer.CustomData.y.");
        }
        else if (HasShadingModel(ShadingModels, MSM_Eye))
        {
            OutSemanticName = TEXT("IrisDistance");
            OutSemanticRole = TEXT("eye_custom_data");
        }
        else
        {
            OutSemanticRole = TEXT("shading_model_custom_data");
        }
    }
    else if (Property == MP_SubsurfaceColor)
    {
        if (HasShadingModel(ShadingModels, MSM_TwoSidedFoliage))
        {
            OutSemanticName = TEXT("TwoSidedFoliageTransmissionColor");
            OutSemanticRole = TEXT("foliage_transmission");
        }
        else if (HasShadingModel(ShadingModels, MSM_Cloth))
        {
            OutSemanticName = TEXT("ClothFuzzColor");
            OutSemanticRole = TEXT("cloth_subsurface_fuzz");
        }
        else if (HasShadingModel(ShadingModels, MSM_Subsurface) || HasShadingModel(ShadingModels, MSM_PreintegratedSkin))
        {
            OutSemanticName = TEXT("SubsurfaceColor");
            OutSemanticRole = TEXT("subsurface_scattering_color");
        }
    }
    else if (Property == MP_Opacity)
    {
        if (HasShadingModel(ShadingModels, MSM_SingleLayerWater))
        {
            OutSemanticName = TEXT("SingleLayerWaterOpacity");
            OutSemanticRole = TEXT("water_volume_absorption_hint");
        }
        else if (BlendMode != BLEND_Opaque && BlendMode != BLEND_Masked)
        {
            OutSemanticName = TEXT("TranslucencyOpacity");
            OutSemanticRole = TEXT("translucent_blend_alpha");
        }
    }
    else if (Property == MP_MaterialAttributes)
    {
        OutSemanticName = TEXT("MaterialAttributesProxy");
        OutSemanticRole = TEXT("packed_material_attributes");
        OutShaderHint = TEXT("Root property is a packed proxy; expand Make/BreakMaterialAttributes or MaterialFunctionCall to inspect individual channels.");
    }
    else if (Property == MP_FrontMaterial)
    {
        OutSemanticName = TEXT("SubstrateFrontMaterial");
        OutSemanticRole = TEXT("substrate_material_tree");
    }
}

TArray<TSharedPtr<FJsonValue>> SemanticPropertiesToJson(UMaterial* Material, UMaterialInterface* DerivedMaterial)
{
    TArray<TSharedPtr<FJsonValue>> Properties;
    if (!Material)
    {
        return Properties;
    }

    UEnum* MaterialPropertyEnum = StaticEnum<EMaterialProperty>();
    const UMaterialInterface* ActiveMaterial = DerivedMaterial ? DerivedMaterial : Material;
    const EMaterialDomain Domain = Material->MaterialDomain.GetValue();
    const EBlendMode BlendMode = ActiveMaterial ? ActiveMaterial->GetBlendMode() : Material->BlendMode.GetValue();
    const FMaterialShadingModelField ShadingModels = ActiveMaterial ? ActiveMaterial->GetShadingModels() : Material->GetShadingModels();

    for (int32 PropertyIndex = 0; PropertyIndex < static_cast<int32>(MP_MAX); ++PropertyIndex)
    {
        const EMaterialProperty Property = static_cast<EMaterialProperty>(PropertyIndex);
        FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
        const bool bActive = ActiveMaterial ? Material->IsPropertyActiveInDerived(Property, ActiveMaterial) : Material->IsPropertyActiveInEditor(Property);
        const bool bConnected = Input && Input->Expression;
        const bool bInteresting =
            Property == MP_CustomData0 ||
            Property == MP_CustomData1 ||
            Property == MP_SubsurfaceColor ||
            Property == MP_Opacity ||
            Property == MP_MaterialAttributes ||
            Property == MP_FrontMaterial;

        if (!Input && !bActive && !bInteresting)
        {
            continue;
        }

        FString SemanticName;
        FString SemanticRole;
        FString ShaderHint;
        ResolveMaterialPropertySemantic(Property, Domain, BlendMode, ShadingModels, SemanticName, SemanticRole, ShaderHint);

        TSharedPtr<FJsonObject> Object = MaterialPropertyConnectionToJson(Material, Property);
        Object->SetBoolField(TEXT("active"), bActive);
        Object->SetBoolField(TEXT("connected"), bConnected);
        Object->SetStringField(TEXT("semantic_name"), SemanticName);
        Object->SetStringField(TEXT("semantic_role"), SemanticRole);
        Object->SetStringField(TEXT("shader_hint"), ShaderHint);
        Object->SetStringField(TEXT("property_enum"), MaterialPropertyEnum ? MaterialPropertyEnum->GetNameStringByValue(static_cast<int64>(Property)) : FString::FromInt(PropertyIndex));
        Properties.Add(MakeShared<FJsonValueObject>(Object));
    }

    return Properties;
}

TSharedPtr<FJsonObject> CostHintsToJson(TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    TSet<FString> UniqueTextures;
    int32 TextureSampleCount = 0;
    int32 TextureObjectCount = 0;
    int32 UniqueSamplerEstimate = 0;
    int32 SharedSamplerCount = 0;
    int32 FunctionCallCount = 0;
    int32 StaticSwitchCount = 0;
    int32 RerouteCount = 0;
    int32 CustomHlslCount = 0;

    TArray<TSharedPtr<FJsonValue>> TextureNodes;
    UEnum* SamplerSourceEnum = StaticEnum<ESamplerSourceMode>();
    UEnum* SamplerTypeEnum = StaticEnum<EMaterialSamplerType>();

    for (const TObjectPtr<UMaterialExpression>& ExpressionPtr : Expressions)
    {
        UMaterialExpression* Expression = ExpressionPtr.Get();
        if (!Expression)
        {
            continue;
        }

        if (Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
        {
            ++FunctionCallCount;
        }
        if (Cast<UMaterialExpressionStaticBoolParameter>(Expression) || Expression->GetClass()->GetName().Contains(TEXT("StaticSwitch")))
        {
            ++StaticSwitchCount;
        }
        if (Cast<UMaterialExpressionRerouteBase>(Expression))
        {
            ++RerouteCount;
        }
        if (Expression->GetClass()->GetName().Contains(TEXT("MaterialExpressionCustom")))
        {
            ++CustomHlslCount;
        }

        if (!Expression->CanReferenceTexture())
        {
            continue;
        }

        const bool bIsTextureSample = Cast<UMaterialExpressionTextureSample>(Expression) != nullptr;
        TextureSampleCount += bIsTextureSample ? 1 : 0;
        TextureObjectCount += bIsTextureSample ? 0 : 1;

        TSharedPtr<FJsonObject> TextureObject = MakeShared<FJsonObject>();
        AddExpressionReferenceFields(TextureObject, Expression, TEXT(""));
        TextureObject->SetBoolField(TEXT("is_texture_sample"), bIsTextureSample);

        TArray<TSharedPtr<FJsonValue>> References;
        for (UObject* ReferencedObject : Expression->GetReferencedTextures())
        {
            if (!ReferencedObject)
            {
                continue;
            }
            UniqueTextures.Add(ReferencedObject->GetPathName());
            References.Add(MakeShared<FJsonValueString>(ReferencedObject->GetPathName()));
        }
        TextureObject->SetArrayField(TEXT("textures"), References);

        if (UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(Expression))
        {
            const ESamplerSourceMode SamplerSource = TextureSample->SamplerSource.GetValue();
            TextureObject->SetStringField(TEXT("sampler_source"), EnumNameOrValue(SamplerSourceEnum, static_cast<int64>(SamplerSource)));
            TextureObject->SetStringField(TEXT("sampler_type"), EnumNameOrValue(SamplerTypeEnum, static_cast<int64>(TextureSample->SamplerType.GetValue())));
            if (SamplerSource == SSM_FromTextureAsset)
            {
                ++UniqueSamplerEstimate;
            }
            else
            {
                ++SharedSamplerCount;
            }
        }
        TextureNodes.Add(MakeShared<FJsonValueObject>(TextureObject));
    }

    Object->SetNumberField(TEXT("texture_sample_count"), TextureSampleCount);
    Object->SetNumberField(TEXT("texture_object_or_parameter_count"), TextureObjectCount);
    Object->SetNumberField(TEXT("unique_texture_count"), UniqueTextures.Num());
    Object->SetNumberField(TEXT("unique_sampler_estimate"), UniqueSamplerEstimate);
    Object->SetNumberField(TEXT("shared_sampler_count"), SharedSamplerCount);
    Object->SetNumberField(TEXT("material_function_call_count"), FunctionCallCount);
    Object->SetNumberField(TEXT("static_switch_count"), StaticSwitchCount);
    Object->SetNumberField(TEXT("reroute_count"), RerouteCount);
    Object->SetNumberField(TEXT("custom_hlsl_count"), CustomHlslCount);
    Object->SetStringField(TEXT("cost_model"), TEXT("graph_hint_only"));
    Object->SetArrayField(TEXT("texture_nodes"), TextureNodes);
    return Object;
}

TSharedPtr<FJsonObject> UsageHintsToJson(UObject* Asset, int32 MaxReferencers)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    if (!Asset)
    {
        return Object;
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FName> ReferencerPackages;
    AssetRegistryModule.Get().GetReferencers(FName(*Asset->GetOutermost()->GetName()), ReferencerPackages);

    Object->SetNumberField(TEXT("referencer_package_count"), ReferencerPackages.Num());
    Object->SetStringField(TEXT("source"), TEXT("AssetRegistry package referencers"));

    TArray<TSharedPtr<FJsonValue>> Referencers;
    const int32 Limit = MaxReferencers > 0 ? FMath::Min(MaxReferencers, ReferencerPackages.Num()) : ReferencerPackages.Num();
    for (int32 Index = 0; Index < Limit; ++Index)
    {
        TSharedPtr<FJsonObject> ReferencerObject = MakeShared<FJsonObject>();
        ReferencerObject->SetStringField(TEXT("package"), ReferencerPackages[Index].ToString());

        TArray<FAssetData> AssetsInPackage;
        AssetRegistryModule.Get().GetAssetsByPackageName(ReferencerPackages[Index], AssetsInPackage);
        TArray<TSharedPtr<FJsonValue>> Assets;
        for (const FAssetData& AssetData : AssetsInPackage)
        {
            TSharedPtr<FJsonObject> AssetObject = MakeShared<FJsonObject>();
            AssetObject->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
            AssetObject->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
            AssetObject->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
            Assets.Add(MakeShared<FJsonValueObject>(AssetObject));
        }
        ReferencerObject->SetArrayField(TEXT("assets"), Assets);
        Referencers.Add(MakeShared<FJsonValueObject>(ReferencerObject));
    }
    Object->SetArrayField(TEXT("referencers"), Referencers);
    Object->SetBoolField(TEXT("truncated"), Limit < ReferencerPackages.Num());
    return Object;
}

TSharedPtr<FJsonObject> MaterialInstanceToJson(UMaterialInstance* Instance)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    if (!Instance)
    {
        return Object;
    }

    Object->SetStringField(TEXT("parent"), ObjectPathOrEmpty(Instance->Parent));

    TArray<TSharedPtr<FJsonValue>> ParentChain;
    UMaterialInterface* Current = Instance;
    TSet<UMaterialInterface*> Visited;
    while (UMaterialInstance* CurrentInstance = Cast<UMaterialInstance>(Current))
    {
        if (Visited.Contains(CurrentInstance))
        {
            TSharedPtr<FJsonObject> LoopObject = MakeShared<FJsonObject>();
            LoopObject->SetStringField(TEXT("asset"), CurrentInstance->GetPathName());
            LoopObject->SetBoolField(TEXT("cycle_detected"), true);
            ParentChain.Add(MakeShared<FJsonValueObject>(LoopObject));
            break;
        }
        Visited.Add(CurrentInstance);

        TSharedPtr<FJsonObject> LinkObject = MakeShared<FJsonObject>();
        LinkObject->SetStringField(TEXT("asset"), CurrentInstance->GetPathName());
        LinkObject->SetStringField(TEXT("class"), CurrentInstance->GetClass()->GetName());
        LinkObject->SetStringField(TEXT("parent"), ObjectPathOrEmpty(CurrentInstance->Parent));
        ParentChain.Add(MakeShared<FJsonValueObject>(LinkObject));
        Current = CurrentInstance->Parent;
    }
    if (Current)
    {
        TSharedPtr<FJsonObject> BaseObject = MakeShared<FJsonObject>();
        BaseObject->SetStringField(TEXT("asset"), Current->GetPathName());
        BaseObject->SetStringField(TEXT("class"), Current->GetClass()->GetName());
        BaseObject->SetBoolField(TEXT("base"), true);
        ParentChain.Add(MakeShared<FJsonValueObject>(BaseObject));
    }
    Object->SetArrayField(TEXT("parent_chain"), ParentChain);

    TArray<TSharedPtr<FJsonValue>> Scalars;
    for (const FScalarParameterValue& Parameter : Instance->ScalarParameterValues)
    {
        TSharedPtr<FJsonObject> ParameterObject = MakeShared<FJsonObject>();
        ParameterObject->SetObjectField(TEXT("parameter"), ParameterInfoToJson(Parameter.ParameterInfo));
        ParameterObject->SetNumberField(TEXT("value"), Parameter.ParameterValue);
        ParameterObject->SetStringField(TEXT("expression_guid"), Parameter.ExpressionGUID.ToString());
        Scalars.Add(MakeShared<FJsonValueObject>(ParameterObject));
    }
    Object->SetArrayField(TEXT("scalar_overrides"), Scalars);

    TArray<TSharedPtr<FJsonValue>> Vectors;
    for (const FVectorParameterValue& Parameter : Instance->VectorParameterValues)
    {
        TSharedPtr<FJsonObject> ParameterObject = MakeShared<FJsonObject>();
        ParameterObject->SetObjectField(TEXT("parameter"), ParameterInfoToJson(Parameter.ParameterInfo));
        ParameterObject->SetObjectField(TEXT("value"), LinearColorToJson(Parameter.ParameterValue));
        ParameterObject->SetStringField(TEXT("expression_guid"), Parameter.ExpressionGUID.ToString());
        Vectors.Add(MakeShared<FJsonValueObject>(ParameterObject));
    }
    Object->SetArrayField(TEXT("vector_overrides"), Vectors);

    TArray<TSharedPtr<FJsonValue>> DoubleVectors;
    for (const FDoubleVectorParameterValue& Parameter : Instance->DoubleVectorParameterValues)
    {
        TSharedPtr<FJsonObject> ParameterObject = MakeShared<FJsonObject>();
        ParameterObject->SetObjectField(TEXT("parameter"), ParameterInfoToJson(Parameter.ParameterInfo));
        ParameterObject->SetObjectField(TEXT("value"), Vector4dToJson(Parameter.ParameterValue));
        ParameterObject->SetStringField(TEXT("expression_guid"), Parameter.ExpressionGUID.ToString());
        DoubleVectors.Add(MakeShared<FJsonValueObject>(ParameterObject));
    }
    Object->SetArrayField(TEXT("double_vector_overrides"), DoubleVectors);

    TArray<TSharedPtr<FJsonValue>> Textures;
    for (const FTextureParameterValue& Parameter : Instance->TextureParameterValues)
    {
        TSharedPtr<FJsonObject> ParameterObject = MakeShared<FJsonObject>();
        ParameterObject->SetObjectField(TEXT("parameter"), ParameterInfoToJson(Parameter.ParameterInfo));
        ParameterObject->SetStringField(TEXT("value"), ObjectPathOrEmpty(Parameter.ParameterValue));
        ParameterObject->SetStringField(TEXT("expression_guid"), Parameter.ExpressionGUID.ToString());
        Textures.Add(MakeShared<FJsonValueObject>(ParameterObject));
    }
    Object->SetArrayField(TEXT("texture_overrides"), Textures);

    TArray<TSharedPtr<FJsonValue>> StaticSwitches;
    TArray<FMaterialParameterInfo> StaticSwitchInfos;
    TArray<FGuid> StaticSwitchIds;
    Instance->GetAllStaticSwitchParameterInfo(StaticSwitchInfos, StaticSwitchIds);
    for (int32 Index = 0; Index < StaticSwitchInfos.Num(); ++Index)
    {
        bool bValue = false;
        FGuid ExpressionGuid;
        const bool bHasValue = Instance->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(StaticSwitchInfos[Index]), bValue, ExpressionGuid, false);
        bool bOverrideValue = false;
        FGuid OverrideGuid;
        const bool bOverridden = Instance->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(StaticSwitchInfos[Index]), bOverrideValue, OverrideGuid, true);

        TSharedPtr<FJsonObject> ParameterObject = MakeShared<FJsonObject>();
        ParameterObject->SetObjectField(TEXT("parameter"), ParameterInfoToJson(StaticSwitchInfos[Index]));
        ParameterObject->SetStringField(TEXT("declared_guid"), StaticSwitchIds.IsValidIndex(Index) ? StaticSwitchIds[Index].ToString() : FString());
        ParameterObject->SetBoolField(TEXT("resolved"), bHasValue);
        ParameterObject->SetBoolField(TEXT("value"), bValue);
        ParameterObject->SetBoolField(TEXT("overridden"), bOverridden);
        if (bOverridden)
        {
            ParameterObject->SetBoolField(TEXT("override_value"), bOverrideValue);
            ParameterObject->SetStringField(TEXT("override_expression_guid"), OverrideGuid.ToString());
        }
        else
        {
            ParameterObject->SetStringField(TEXT("expression_guid"), ExpressionGuid.ToString());
        }
        StaticSwitches.Add(MakeShared<FJsonValueObject>(ParameterObject));
    }
    Object->SetArrayField(TEXT("static_switches"), StaticSwitches);

    Object->SetNumberField(TEXT("scalar_override_count"), Scalars.Num());
    Object->SetNumberField(TEXT("vector_override_count"), Vectors.Num());
    Object->SetNumberField(TEXT("texture_override_count"), Textures.Num());
    Object->SetNumberField(TEXT("static_switch_count"), StaticSwitches.Num());
    return Object;
}

TSharedPtr<FJsonObject> GraphSummaryToJson(const FMaterialGraphTarget& Target, bool bIncludeNodes)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("graph_type"), Target.GetGraphType());
    Object->SetNumberField(TEXT("node_count"), Target.GetExpressions().Num());
    Object->SetObjectField(TEXT("cost_hints"), CostHintsToJson(Target.GetExpressions()));

    TArray<TSharedPtr<FJsonValue>> FunctionCalls;
    TArray<TSharedPtr<FJsonValue>> Reroutes;
    TArray<TSharedPtr<FJsonValue>> CustomOutputs;
    TArray<TSharedPtr<FJsonValue>> CustomHlslNodes;
    TArray<TSharedPtr<FJsonValue>> StaticSwitches;
    TArray<TSharedPtr<FJsonValue>> Nodes;

    for (const TObjectPtr<UMaterialExpression>& ExpressionPtr : Target.GetExpressions())
    {
        UMaterialExpression* Expression = ExpressionPtr.Get();
        if (!Expression)
        {
            continue;
        }

        if (bIncludeNodes)
        {
            Nodes.Add(MakeShared<FJsonValueObject>(ExpressionToJson(Expression, true, Target.GetExpressions())));
        }
        if (UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
        {
            TSharedPtr<FJsonObject> ObjectValue = MakeShared<FJsonObject>();
            AddExpressionReferenceFields(ObjectValue, Expression, TEXT(""));
            ObjectValue->SetObjectField(TEXT("function_call"), FunctionCallToJson(FunctionCall));
            FunctionCalls.Add(MakeShared<FJsonValueObject>(ObjectValue));
        }
        if (UMaterialExpressionRerouteBase* Reroute = Cast<UMaterialExpressionRerouteBase>(Expression))
        {
            TSharedPtr<FJsonObject> ObjectValue = MakeShared<FJsonObject>();
            AddExpressionReferenceFields(ObjectValue, Expression, TEXT(""));
            ObjectValue->SetObjectField(TEXT("reroute"), RerouteToJson(Reroute, Target.GetExpressions()));
            Reroutes.Add(MakeShared<FJsonValueObject>(ObjectValue));
        }
        if (Expression->GetClass()->GetName().Contains(TEXT("CustomOutput")))
        {
            TSharedPtr<FJsonObject> ObjectValue = MakeShared<FJsonObject>();
            AddExpressionReferenceFields(ObjectValue, Expression, TEXT(""));
            ObjectValue->SetStringField(TEXT("class"), Expression->GetClass()->GetName());
            CustomOutputs.Add(MakeShared<FJsonValueObject>(ObjectValue));
        }
        if (Expression->GetClass()->GetName().Contains(TEXT("MaterialExpressionCustom")))
        {
            TSharedPtr<FJsonObject> ObjectValue = MakeShared<FJsonObject>();
            AddExpressionReferenceFields(ObjectValue, Expression, TEXT(""));
            ObjectValue->SetObjectField(TEXT("custom_hlsl"), CustomHlslToJson(Expression));
            CustomHlslNodes.Add(MakeShared<FJsonValueObject>(ObjectValue));
        }
        if (Cast<UMaterialExpressionStaticBoolParameter>(Expression) || Expression->GetClass()->GetName().Contains(TEXT("StaticSwitch")))
        {
            TSharedPtr<FJsonObject> ObjectValue = MakeShared<FJsonObject>();
            AddExpressionReferenceFields(ObjectValue, Expression, TEXT(""));
            ObjectValue->SetObjectField(TEXT("static_switch"), StaticSwitchToJson(Expression));
            StaticSwitches.Add(MakeShared<FJsonValueObject>(ObjectValue));
        }
    }

    Object->SetArrayField(TEXT("function_calls"), FunctionCalls);
    Object->SetArrayField(TEXT("reroutes"), Reroutes);
    Object->SetArrayField(TEXT("custom_outputs"), CustomOutputs);
    Object->SetArrayField(TEXT("custom_hlsl_nodes"), CustomHlslNodes);
    Object->SetArrayField(TEXT("static_switches"), StaticSwitches);
    if (bIncludeNodes)
    {
        Object->SetArrayField(TEXT("nodes"), Nodes);
    }
    return Object;
}

bool IsEngineOrScriptAssetPath(const FString& AssetPath)
{
    return AssetPath.StartsWith(TEXT("/Engine/")) ||
        AssetPath.StartsWith(TEXT("/Script/"));
}

int32 CountMaterialFunctionCalls(UMaterial* Material)
{
    if (!Material)
    {
        return 0;
    }

    int32 Count = 0;
    for (const TObjectPtr<UMaterialExpression>& ExpressionPtr : Material->GetExpressions())
    {
        if (Cast<UMaterialExpressionMaterialFunctionCall>(ExpressionPtr.Get()))
        {
            ++Count;
        }
    }
    return Count;
}

TArray<FString> GetExpandableFunctionCallNodeIds(
    UMaterial* Material,
    bool bExcludeEngineFunctions,
    TArray<TSharedPtr<FJsonValue>>& OutSkipped)
{
    TArray<FString> NodeIds;
    if (!Material)
    {
        return NodeIds;
    }

    for (const TObjectPtr<UMaterialExpression>& ExpressionPtr : Material->GetExpressions())
    {
        UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(ExpressionPtr.Get());
        if (!FunctionCall)
        {
            continue;
        }

        UMaterialFunctionInterface* MaterialFunction = FunctionCall->MaterialFunction;
        const FString FunctionPath = MaterialFunction ? MaterialFunction->GetPathName() : FString();
        if (!MaterialFunction)
        {
            TSharedPtr<FJsonObject> SkippedObject = MakeShared<FJsonObject>();
            AddExpressionReferenceFields(SkippedObject, FunctionCall, TEXT(""));
            SkippedObject->SetStringField(TEXT("reason"), TEXT("missing_material_function"));
            OutSkipped.Add(MakeShared<FJsonValueObject>(SkippedObject));
            continue;
        }

        if (bExcludeEngineFunctions && IsEngineOrScriptAssetPath(FunctionPath))
        {
            TSharedPtr<FJsonObject> SkippedObject = MakeShared<FJsonObject>();
            AddExpressionReferenceFields(SkippedObject, FunctionCall, TEXT(""));
            SkippedObject->SetStringField(TEXT("function"), FunctionPath);
            SkippedObject->SetStringField(TEXT("reason"), TEXT("engine_or_script_function"));
            OutSkipped.Add(MakeShared<FJsonValueObject>(SkippedObject));
            continue;
        }

        NodeIds.Add(GetExpressionId(FunctionCall));
    }

    return NodeIds;
}

UWorld* GetEditorOrPIEWorld()
{
    if (!GEditor)
    {
        return nullptr;
    }

    for (const FWorldContext& WorldContext : GEditor->GetWorldContexts())
    {
        if (WorldContext.World() && WorldContext.WorldType == EWorldType::PIE)
        {
            return WorldContext.World();
        }
    }

    return GEditor->GetEditorWorldContext().World();
}

FString GetWorldTypeName(const UWorld* World)
{
    if (!World)
    {
        return TEXT("");
    }

    switch (World->WorldType)
    {
    case EWorldType::Editor:
        return TEXT("Editor");
    case EWorldType::PIE:
        return TEXT("PIE");
    case EWorldType::Game:
        return TEXT("Game");
    case EWorldType::GamePreview:
        return TEXT("GamePreview");
    case EWorldType::EditorPreview:
        return TEXT("EditorPreview");
    case EWorldType::Inactive:
        return TEXT("Inactive");
    default:
        return TEXT("Unknown");
    }
}

bool ShouldIncludeNamedParameter(const TSet<FName>& ParameterFilter, const FName& ParameterName)
{
    return ParameterFilter.Num() == 0 || ParameterFilter.Contains(ParameterName);
}

TSet<FName> GetOptionalParameterNameFilter(const TSharedPtr<FJsonObject>& Params)
{
    TSet<FName> ParameterFilter;
    const TArray<TSharedPtr<FJsonValue>>* ParameterNames = nullptr;
    if (!Params->TryGetArrayField(TEXT("parameter_names"), ParameterNames))
    {
        return ParameterFilter;
    }

    for (const TSharedPtr<FJsonValue>& Value : *ParameterNames)
    {
        if (!Value.IsValid())
        {
            continue;
        }

        const FString Name = Value->AsString().TrimStartAndEnd();
        if (!Name.IsEmpty())
        {
            ParameterFilter.Add(FName(*Name));
        }
    }

    return ParameterFilter;
}

struct FExpandedOutputReplacement
{
    bool bValid = false;
    FExpressionInput Input;
};

struct FMaterialFunctionExpansionStats
{
    int32 DuplicatedNodeCount = 0;
    int32 PreviewDefaultNodeCount = 0;
    int32 RewiredFunctionInputCount = 0;
    int32 RewiredConsumerCount = 0;
    TArray<FString> CreatedFunctionCallNodeIds;
};

const FFunctionExpressionInput* FindFunctionCallInputById(
    const UMaterialExpressionMaterialFunctionCall* FunctionCall,
    const FGuid& InputId)
{
    if (!FunctionCall)
    {
        return nullptr;
    }

    for (const FFunctionExpressionInput& FunctionInput : FunctionCall->FunctionInputs)
    {
        if (FunctionInput.ExpressionInputId == InputId)
        {
            return &FunctionInput;
        }
    }

    return nullptr;
}

void CollectFunctionIoExpressions(
    UMaterialFunctionInterface* MaterialFunction,
    TMap<FGuid, UMaterialExpressionFunctionInput*>& OutInputsById,
    TMap<FGuid, UMaterialExpressionFunctionOutput*>& OutOutputsById)
{
    if (!MaterialFunction)
    {
        return;
    }

    for (const TObjectPtr<UMaterialExpression>& ExpressionPtr : MaterialFunction->GetExpressions())
    {
        UMaterialExpression* Expression = ExpressionPtr.Get();
        if (!Expression)
        {
            continue;
        }

        if (UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(Expression))
        {
            OutInputsById.Add(FunctionInput->Id, FunctionInput);
        }
        else if (UMaterialExpressionFunctionOutput* FunctionOutput = Cast<UMaterialExpressionFunctionOutput>(Expression))
        {
            OutOutputsById.Add(FunctionOutput->Id, FunctionOutput);
        }
    }
}

UMaterialExpression* CreatePreviewDefaultExpression(
    UMaterial* Material,
    const UMaterialExpressionFunctionInput* FunctionInput,
    const FVector2D& PastePositionOffset,
    TArray<UMaterialExpression*>& CreatedExpressions,
    FMaterialFunctionExpansionStats& Stats)
{
    if (!Material || !FunctionInput)
    {
        return nullptr;
    }

    const int32 NodeX = FMath::RoundToInt(FunctionInput->MaterialExpressionEditorX + PastePositionOffset.X);
    const int32 NodeY = FMath::RoundToInt(FunctionInput->MaterialExpressionEditorY + PastePositionOffset.Y);

    switch (FunctionInput->InputType.GetValue())
    {
    case FunctionInput_Scalar:
    {
        UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(
            UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant::StaticClass(), NodeX, NodeY));
        if (Constant)
        {
            Constant->R = FunctionInput->PreviewValue.X;
            CreatedExpressions.Add(Constant);
            ++Stats.PreviewDefaultNodeCount;
        }
        return Constant;
    }
    case FunctionInput_Vector2:
    {
        UMaterialExpressionConstant2Vector* Constant = Cast<UMaterialExpressionConstant2Vector>(
            UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant2Vector::StaticClass(), NodeX, NodeY));
        if (Constant)
        {
            Constant->R = FunctionInput->PreviewValue.X;
            Constant->G = FunctionInput->PreviewValue.Y;
            CreatedExpressions.Add(Constant);
            ++Stats.PreviewDefaultNodeCount;
        }
        return Constant;
    }
    case FunctionInput_Vector3:
    {
        UMaterialExpressionConstant3Vector* Constant = Cast<UMaterialExpressionConstant3Vector>(
            UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant3Vector::StaticClass(), NodeX, NodeY));
        if (Constant)
        {
            Constant->Constant = FLinearColor(FunctionInput->PreviewValue);
            CreatedExpressions.Add(Constant);
            ++Stats.PreviewDefaultNodeCount;
        }
        return Constant;
    }
    case FunctionInput_Vector4:
    {
        UMaterialExpressionConstant4Vector* Constant = Cast<UMaterialExpressionConstant4Vector>(
            UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant4Vector::StaticClass(), NodeX, NodeY));
        if (Constant)
        {
            Constant->Constant = FLinearColor(FunctionInput->PreviewValue);
            CreatedExpressions.Add(Constant);
            ++Stats.PreviewDefaultNodeCount;
        }
        return Constant;
    }
    case FunctionInput_MaterialAttributes:
    {
        UMaterialExpressionMakeMaterialAttributes* MakeAttributes = Cast<UMaterialExpressionMakeMaterialAttributes>(
            UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMakeMaterialAttributes::StaticClass(), NodeX, NodeY));
        if (!MakeAttributes)
        {
            return nullptr;
        }

        CreatedExpressions.Add(MakeAttributes);
        ++Stats.PreviewDefaultNodeCount;

        UMaterialExpressionConstant3Vector* EmissivePreview = Cast<UMaterialExpressionConstant3Vector>(
            UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant3Vector::StaticClass(), NodeX - 240, NodeY));
        if (EmissivePreview)
        {
            EmissivePreview->Constant = FLinearColor(FunctionInput->PreviewValue);
            MakeAttributes->EmissiveColor.Expression = EmissivePreview;
            MakeAttributes->EmissiveColor.OutputIndex = 0;
            CreatedExpressions.Add(EmissivePreview);
            ++Stats.PreviewDefaultNodeCount;
        }

        return MakeAttributes;
    }
    default:
        return nullptr;
    }
}

bool ResolveCopiedExpressionInput(
    UMaterial* Material,
    const UMaterialExpressionMaterialFunctionCall* FunctionCall,
    const TMap<UMaterialExpression*, UMaterialExpression*>& CopiedExpressions,
    const FVector2D& PastePositionOffset,
    TArray<UMaterialExpression*>& CreatedExpressions,
    FExpressionInput& Input,
    FMaterialFunctionExpansionStats& Stats,
    TArray<FString>& Errors);

bool BuildFunctionInputReplacement(
    UMaterial* Material,
    const UMaterialExpressionMaterialFunctionCall* FunctionCall,
    const UMaterialExpressionFunctionInput* FunctionInput,
    const TMap<UMaterialExpression*, UMaterialExpression*>& CopiedExpressions,
    const FVector2D& PastePositionOffset,
    TArray<UMaterialExpression*>& CreatedExpressions,
    FExpressionInput& OutInput,
    FMaterialFunctionExpansionStats& Stats,
    TArray<FString>& Errors)
{
    if (!FunctionInput)
    {
        Errors.Add(TEXT("Function input expression was missing while expanding material function call."));
        return false;
    }

    if (const FFunctionExpressionInput* FunctionCallInput = FindFunctionCallInputById(FunctionCall, FunctionInput->Id))
    {
        if (FunctionCallInput->Input.Expression)
        {
            OutInput = FunctionCallInput->Input;
            return true;
        }
    }

    if (!FunctionInput->bUsePreviewValueAsDefault)
    {
        Errors.Add(FString::Printf(
            TEXT("Function input '%s' is unconnected and does not use preview as default."),
            *FunctionInput->InputName.ToString()));
        return false;
    }

    if (FunctionInput->Preview.Expression)
    {
        OutInput = FunctionInput->Preview;
        if (ResolveCopiedExpressionInput(Material, FunctionCall, CopiedExpressions, PastePositionOffset, CreatedExpressions, OutInput, Stats, Errors))
        {
            return true;
        }
    }

    UMaterialExpression* PreviewExpression = CreatePreviewDefaultExpression(Material, FunctionInput, PastePositionOffset, CreatedExpressions, Stats);
    if (!PreviewExpression)
    {
        Errors.Add(FString::Printf(
            TEXT("Function input '%s' requires unsupported preview default type %d."),
            *FunctionInput->InputName.ToString(),
            static_cast<int32>(FunctionInput->InputType.GetValue())));
        return false;
    }

    OutInput = FExpressionInput();
    OutInput.Expression = PreviewExpression;
    OutInput.OutputIndex = 0;
    return true;
}

bool ResolveCopiedExpressionInput(
    UMaterial* Material,
    const UMaterialExpressionMaterialFunctionCall* FunctionCall,
    const TMap<UMaterialExpression*, UMaterialExpression*>& CopiedExpressions,
    const FVector2D& PastePositionOffset,
    TArray<UMaterialExpression*>& CreatedExpressions,
    FExpressionInput& Input,
    FMaterialFunctionExpansionStats& Stats,
    TArray<FString>& Errors)
{
    UMaterialExpression* SourceExpression = Input.Expression;
    if (!SourceExpression)
    {
        return true;
    }

    if (UMaterialExpression* const* CopiedExpression = CopiedExpressions.Find(SourceExpression))
    {
        Input.Expression = *CopiedExpression;
        return true;
    }

    if (UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(SourceExpression))
    {
        FExpressionInput ReplacementInput;
        if (!BuildFunctionInputReplacement(Material, FunctionCall, FunctionInput, CopiedExpressions, PastePositionOffset, CreatedExpressions, ReplacementInput, Stats, Errors))
        {
            return false;
        }

        Input = ReplacementInput;
        ++Stats.RewiredFunctionInputCount;
        return true;
    }

    Errors.Add(FString::Printf(
        TEXT("Expression input still points to uncopied function expression '%s'."),
        *SourceExpression->GetPathName()));
    return false;
}

void FindFunctionCallReferencedOutputIndices(
    UMaterial* Material,
    UMaterialExpressionMaterialFunctionCall* FunctionCall,
    TSet<int32>& OutReferencedOutputIndices)
{
    if (!Material || !FunctionCall)
    {
        return;
    }

    auto VisitInput = [FunctionCall, &OutReferencedOutputIndices](FExpressionInput* Input)
    {
        if (!Input || Input->Expression != FunctionCall)
        {
            return;
        }

        int32 OutputIndex = Input->OutputIndex;
        if (!FunctionCall->FunctionOutputs.IsValidIndex(OutputIndex) && FunctionCall->FunctionOutputs.Num() == 1)
        {
            OutputIndex = 0;
        }
        OutReferencedOutputIndices.Add(OutputIndex);
    };

    for (const TObjectPtr<UMaterialExpression>& ExpressionPtr : Material->GetExpressions())
    {
        UMaterialExpression* Expression = ExpressionPtr.Get();
        if (!Expression)
        {
            continue;
        }

        for (FExpressionInputIterator It(Expression); It; ++It)
        {
            VisitInput(It.Input);
        }
    }

    for (int32 PropertyIndex = 0; PropertyIndex < MP_MAX; ++PropertyIndex)
    {
        VisitInput(Material->GetExpressionInputForProperty(static_cast<EMaterialProperty>(PropertyIndex)));
    }
}

int32 RewireFunctionCallConsumers(
    UMaterial* Material,
    UMaterialExpressionMaterialFunctionCall* FunctionCall,
    const TArray<FExpandedOutputReplacement>& OutputReplacements)
{
    if (!Material || !FunctionCall)
    {
        return 0;
    }

    int32 RewiredCount = 0;
    auto VisitInput = [FunctionCall, &OutputReplacements, &RewiredCount](FExpressionInput* Input)
    {
        if (!Input || Input->Expression != FunctionCall)
        {
            return;
        }

        int32 OutputIndex = Input->OutputIndex;
        if (!OutputReplacements.IsValidIndex(OutputIndex) && OutputReplacements.Num() == 1)
        {
            OutputIndex = 0;
        }
        if (OutputReplacements.IsValidIndex(OutputIndex) && OutputReplacements[OutputIndex].bValid)
        {
            *Input = OutputReplacements[OutputIndex].Input;
            ++RewiredCount;
        }
    };

    for (const TObjectPtr<UMaterialExpression>& ExpressionPtr : Material->GetExpressions())
    {
        UMaterialExpression* Expression = ExpressionPtr.Get();
        if (!Expression)
        {
            continue;
        }

        for (FExpressionInputIterator It(Expression); It; ++It)
        {
            VisitInput(It.Input);
        }
    }

    for (int32 PropertyIndex = 0; PropertyIndex < MP_MAX; ++PropertyIndex)
    {
        VisitInput(Material->GetExpressionInputForProperty(static_cast<EMaterialProperty>(PropertyIndex)));
    }

    return RewiredCount;
}

void CleanupCreatedMaterialExpressions(UMaterial* Material, TArray<UMaterialExpression*>& CreatedExpressions)
{
    if (!Material)
    {
        return;
    }

    for (int32 Index = CreatedExpressions.Num() - 1; Index >= 0; --Index)
    {
        if (CreatedExpressions[Index])
        {
            UMaterialEditingLibrary::DeleteMaterialExpression(Material, CreatedExpressions[Index]);
        }
    }

    CreatedExpressions.Reset();
}

bool ExpandFunctionCallIntoMaterial(
    UMaterial* Material,
    UMaterialExpressionMaterialFunctionCall* FunctionCall,
    FMaterialFunctionExpansionStats& Stats,
    TArray<FString>& Errors)
{
    if (!Material || !FunctionCall || !FunctionCall->MaterialFunction)
    {
        Errors.Add(TEXT("Missing material, function call, or material function while expanding."));
        return false;
    }

    UMaterialFunctionInterface* MaterialFunction = FunctionCall->MaterialFunction;
    TMap<FGuid, UMaterialExpressionFunctionInput*> FunctionInputsById;
    TMap<FGuid, UMaterialExpressionFunctionOutput*> FunctionOutputsById;
    CollectFunctionIoExpressions(MaterialFunction, FunctionInputsById, FunctionOutputsById);

    TArray<UMaterialExpression*> SourceExpressions;
    FVector2D AveragePosition = FVector2D::ZeroVector;
    for (const TObjectPtr<UMaterialExpression>& ExpressionPtr : MaterialFunction->GetExpressions())
    {
        UMaterialExpression* SourceExpression = ExpressionPtr.Get();
        if (!SourceExpression ||
            Cast<UMaterialExpressionFunctionInput>(SourceExpression) ||
            Cast<UMaterialExpressionFunctionOutput>(SourceExpression))
        {
            continue;
        }

        SourceExpressions.Add(SourceExpression);
        AveragePosition.X += SourceExpression->MaterialExpressionEditorX;
        AveragePosition.Y += SourceExpression->MaterialExpressionEditorY;
    }

    if (SourceExpressions.Num() > 0)
    {
        AveragePosition /= SourceExpressions.Num();
    }

    const FVector2D PastePosition(FunctionCall->MaterialExpressionEditorX, FunctionCall->MaterialExpressionEditorY);
    const FVector2D PastePositionOffset = PastePosition - AveragePosition;
    TMap<UMaterialExpression*, UMaterialExpression*> CopiedExpressions;
    TArray<UMaterialExpression*> CreatedExpressions;
    const int32 InitialErrorCount = Errors.Num();

    for (UMaterialExpression* SourceExpression : SourceExpressions)
    {
        UMaterialExpression* NewExpression = UMaterialEditingLibrary::DuplicateMaterialExpression(Material, nullptr, SourceExpression);
        if (!NewExpression)
        {
            Errors.Add(FString::Printf(TEXT("Failed to duplicate function expression '%s'."), *SourceExpression->GetPathName()));
            CleanupCreatedMaterialExpressions(Material, CreatedExpressions);
            return false;
        }

        NewExpression->MaterialExpressionEditorX = FMath::RoundToInt(SourceExpression->MaterialExpressionEditorX + PastePositionOffset.X);
        NewExpression->MaterialExpressionEditorY = FMath::RoundToInt(SourceExpression->MaterialExpressionEditorY + PastePositionOffset.Y);
        CopiedExpressions.Add(SourceExpression, NewExpression);
        CreatedExpressions.Add(NewExpression);
        ++Stats.DuplicatedNodeCount;
        if (Cast<UMaterialExpressionMaterialFunctionCall>(NewExpression))
        {
            Stats.CreatedFunctionCallNodeIds.Add(GetExpressionId(NewExpression));
        }
    }

    for (UMaterialExpression* NewExpression : CreatedExpressions)
    {
        if (NewExpression)
        {
            NewExpression->PostCopyNode(CreatedExpressions);
        }
    }

    for (const TPair<UMaterialExpression*, UMaterialExpression*>& Pair : CopiedExpressions)
    {
        UMaterialExpression* NewExpression = Pair.Value;
        if (!NewExpression)
        {
            continue;
        }

        for (FExpressionInputIterator It(NewExpression); It; ++It)
        {
            if (!ResolveCopiedExpressionInput(Material, FunctionCall, CopiedExpressions, PastePositionOffset, CreatedExpressions, *It.Input, Stats, Errors))
            {
                CleanupCreatedMaterialExpressions(Material, CreatedExpressions);
                return false;
            }
        }
    }

    TSet<int32> ReferencedOutputIndices;
    FindFunctionCallReferencedOutputIndices(Material, FunctionCall, ReferencedOutputIndices);
    const bool bRequireAllOutputs = ReferencedOutputIndices.Num() == 0;

    TArray<FExpandedOutputReplacement> OutputReplacements;
    OutputReplacements.SetNum(FunctionCall->FunctionOutputs.Num());
    for (int32 OutputIndex = 0; OutputIndex < FunctionCall->FunctionOutputs.Num(); ++OutputIndex)
    {
        if (!bRequireAllOutputs && !ReferencedOutputIndices.Contains(OutputIndex))
        {
            continue;
        }

        const FFunctionExpressionOutput& FunctionCallOutput = FunctionCall->FunctionOutputs[OutputIndex];
        UMaterialExpressionFunctionOutput* FunctionOutput = FunctionCallOutput.ExpressionOutput;
        if (!FunctionOutput)
        {
            FunctionOutput = FunctionOutputsById.FindRef(FunctionCallOutput.ExpressionOutputId);
        }
        if (!FunctionOutput)
        {
            Errors.Add(FString::Printf(TEXT("Function output index %d could not be resolved."), OutputIndex));
            continue;
        }

        FExpressionInput OutputInput = FunctionOutput->A;
        if (!OutputInput.Expression)
        {
            Errors.Add(FString::Printf(TEXT("Function output '%s' is not connected."), *FunctionOutput->OutputName.ToString()));
            continue;
        }

        if (ResolveCopiedExpressionInput(Material, FunctionCall, CopiedExpressions, PastePositionOffset, CreatedExpressions, OutputInput, Stats, Errors))
        {
            OutputReplacements[OutputIndex].bValid = true;
            OutputReplacements[OutputIndex].Input = OutputInput;
        }
    }

    if (Errors.Num() > InitialErrorCount)
    {
        CleanupCreatedMaterialExpressions(Material, CreatedExpressions);
        return false;
    }

    for (int32 ReferencedOutputIndex : ReferencedOutputIndices)
    {
        if (!OutputReplacements.IsValidIndex(ReferencedOutputIndex) || !OutputReplacements[ReferencedOutputIndex].bValid)
        {
            Errors.Add(FString::Printf(TEXT("Function call output index %d is referenced but has no valid expanded replacement."), ReferencedOutputIndex));
            CleanupCreatedMaterialExpressions(Material, CreatedExpressions);
            return false;
        }
    }

    Stats.RewiredConsumerCount += RewireFunctionCallConsumers(Material, FunctionCall, OutputReplacements);
    UMaterialEditingLibrary::DeleteMaterialExpression(Material, FunctionCall);
    CreatedExpressions.Reset();
    return true;
}
}

FUnrealMCPMaterialCommands::FUnrealMCPMaterialCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("resolve_material_graph"))
    {
        return HandleResolveMaterialGraph(Params);
    }
    if (CommandType == TEXT("list_material_nodes"))
    {
        return HandleListMaterialNodes(Params);
    }
    if (CommandType == TEXT("analyze_material_graph"))
    {
        return HandleAnalyzeMaterialGraph(Params);
    }
    if (CommandType == TEXT("add_material_node"))
    {
        return HandleAddMaterialNode(Params);
    }
    if (CommandType == TEXT("add_custom_material_node"))
    {
        return HandleAddCustomMaterialNode(Params);
    }
    if (CommandType == TEXT("set_material_node_property"))
    {
        return HandleSetMaterialNodeProperty(Params);
    }
    if (CommandType == TEXT("connect_material_nodes"))
    {
        return HandleConnectMaterialNodes(Params);
    }
    if (CommandType == TEXT("connect_material_property"))
    {
        return HandleConnectMaterialProperty(Params);
    }
    if (CommandType == TEXT("delete_material_node"))
    {
        return HandleDeleteMaterialNode(Params);
    }
    if (CommandType == TEXT("layout_material_nodes"))
    {
        return HandleLayoutMaterialNodes(Params);
    }
    if (CommandType == TEXT("compile_and_save_material"))
    {
        return HandleCompileAndSaveMaterial(Params);
    }
    if (CommandType == TEXT("expand_material_function_calls"))
    {
        return HandleExpandMaterialFunctionCalls(Params);
    }
    if (CommandType == TEXT("get_material_parameter_collection_values"))
    {
        return HandleGetMaterialParameterCollectionValues(Params);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown material command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleResolveMaterialGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material_path' parameter"));
    }

    FString GraphType;
    Params->TryGetStringField(TEXT("graph_type"), GraphType);

    TArray<FString> CandidatePaths;
    FMaterialGraphTarget Target;
    FString ErrorMessage;
    const bool bResolved = ResolveMaterialGraph(MaterialPath, GraphType, Target, &CandidatePaths, ErrorMessage);

    TArray<TSharedPtr<FJsonValue>> CandidateArray;
    for (const FString& CandidatePath : CandidatePaths)
    {
        CandidateArray.Add(MakeShared<FJsonValueString>(CandidatePath));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("resolved"), bResolved);
    ResultObj->SetArrayField(TEXT("candidates"), CandidateArray);
    if (bResolved)
    {
        ResultObj->SetStringField(TEXT("asset_path"), Target.GetPathName());
        ResultObj->SetStringField(TEXT("name"), Target.Asset ? Target.Asset->GetName() : FString());
        ResultObj->SetStringField(TEXT("graph_type"), Target.GetGraphType());
    }
    else
    {
        ResultObj->SetStringField(TEXT("error"), ErrorMessage);
    }

    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleListMaterialNodes(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material_path' parameter"));
    }

    FString GraphType;
    Params->TryGetStringField(TEXT("graph_type"), GraphType);

    FString NodeType;
    Params->TryGetStringField(TEXT("node_type"), NodeType);

    FString DescContains;
    Params->TryGetStringField(TEXT("desc_contains"), DescContains);

    bool bIncludePins = true;
    if (Params->HasField(TEXT("include_pins")))
    {
        bIncludePins = Params->GetBoolField(TEXT("include_pins"));
    }

    FMaterialGraphTarget Target;
    FString ErrorMessage;
    if (!ResolveMaterialGraph(MaterialPath, GraphType, Target, nullptr, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (const TObjectPtr<UMaterialExpression>& ExpressionPtr : Target.GetExpressions())
    {
        UMaterialExpression* Expression = ExpressionPtr.Get();
        if (!Expression)
        {
            continue;
        }

        if (!NodeType.IsEmpty() &&
            !Expression->GetClass()->GetName().Contains(NodeType) &&
            !Expression->GetName().Contains(NodeType))
        {
            continue;
        }
        if (!DescContains.IsEmpty() && !Expression->Desc.Contains(DescContains))
        {
            continue;
        }

        Nodes.Add(MakeShared<FJsonValueObject>(ExpressionToJson(Expression, bIncludePins, Target.GetExpressions())));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("material"), Target.GetPathName());
    ResultObj->SetStringField(TEXT("graph_type"), Target.GetGraphType());
    ResultObj->SetNumberField(TEXT("node_count"), Nodes.Num());
    ResultObj->SetArrayField(TEXT("nodes"), Nodes);
    ResultObj->SetObjectField(TEXT("cost_hints"), CostHintsToJson(Target.GetExpressions()));

    if (Target.Material)
    {
        ResultObj->SetObjectField(TEXT("material_settings"), MaterialSettingsToJson(Target.Material, Target.Material));
        ResultObj->SetArrayField(TEXT("semantic_properties"), SemanticPropertiesToJson(Target.Material, Target.Material));

        TArray<TSharedPtr<FJsonValue>> PropertyConnections;
        for (int32 PropertyIndex = 0; PropertyIndex < static_cast<int32>(MP_MAX); ++PropertyIndex)
        {
            const EMaterialProperty Property = static_cast<EMaterialProperty>(PropertyIndex);
            if (FExpressionInput* Input = Target.Material->GetExpressionInputForProperty(Property))
            {
                (void)Input;
                PropertyConnections.Add(MakeShared<FJsonValueObject>(MaterialPropertyConnectionToJson(Target.Material, Property)));
            }
        }
        ResultObj->SetArrayField(TEXT("material_properties"), PropertyConnections);
    }

    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleAnalyzeMaterialGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material_path' parameter"));
    }

    FString GraphType;
    Params->TryGetStringField(TEXT("graph_type"), GraphType);

    bool bIncludeNodes = false;
    if (Params->HasField(TEXT("include_nodes")))
    {
        bIncludeNodes = Params->GetBoolField(TEXT("include_nodes"));
    }

    bool bIncludeUsage = true;
    if (Params->HasField(TEXT("include_usage")))
    {
        bIncludeUsage = Params->GetBoolField(TEXT("include_usage"));
    }

    int32 MaxReferencers = 25;
    if (Params->HasField(TEXT("max_referencers")))
    {
        MaxReferencers = static_cast<int32>(Params->GetNumberField(TEXT("max_referencers")));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("analysis_version"), TEXT("material_mcp_analysis_v1"));

    const FString Query = NormalizeObjectPathForLoad(MaterialPath);
    UObject* LoadedObject = LoadObject<UObject>(nullptr, *Query);
    if (!LoadedObject)
    {
        TArray<FString> CandidatePaths = FindMaterialGraphAssetPaths(MaterialPath, GraphType);
        if (NormalizeMaterialGraphType(GraphType) != TEXT("function"))
        {
            AppendAssetCandidatesForClass(UMaterialInstance::StaticClass(), MaterialPath, CandidatePaths);
        }
        if (CandidatePaths.Num() == 1)
        {
            LoadedObject = LoadObject<UObject>(nullptr, *NormalizeObjectPathForLoad(CandidatePaths[0]));
        }
        else if (CandidatePaths.Num() > 1)
        {
            TArray<TSharedPtr<FJsonValue>> CandidateArray;
            for (const FString& CandidatePath : CandidatePaths)
            {
                CandidateArray.Add(MakeShared<FJsonValueString>(CandidatePath));
            }
            TSharedPtr<FJsonObject> ErrorObj = FUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Ambiguous material analysis target '%s'. Use a full object path."), *MaterialPath));
            ErrorObj->SetArrayField(TEXT("candidates"), CandidateArray);
            return ErrorObj;
        }
    }

    if (UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(LoadedObject))
    {
        ResultObj->SetStringField(TEXT("asset_path"), MaterialInstance->GetPathName());
        ResultObj->SetStringField(TEXT("asset_class"), MaterialInstance->GetClass()->GetName());
        ResultObj->SetStringField(TEXT("asset_kind"), TEXT("material_instance"));
        UMaterial* BaseMaterial = MaterialInstance->GetMaterial();
        ResultObj->SetObjectField(TEXT("material_settings"), MaterialSettingsToJson(MaterialInstance, BaseMaterial));
        ResultObj->SetObjectField(TEXT("material_instance"), MaterialInstanceToJson(MaterialInstance));
        if (BaseMaterial)
        {
            ResultObj->SetStringField(TEXT("base_material"), BaseMaterial->GetPathName());
            ResultObj->SetArrayField(TEXT("semantic_properties"), SemanticPropertiesToJson(BaseMaterial, MaterialInstance));
            FMaterialGraphTarget BaseTarget;
            BaseTarget.Asset = BaseMaterial;
            BaseTarget.Material = BaseMaterial;
            ResultObj->SetObjectField(TEXT("base_graph"), GraphSummaryToJson(BaseTarget, bIncludeNodes));
        }
        if (bIncludeUsage)
        {
            ResultObj->SetObjectField(TEXT("usage_hints"), UsageHintsToJson(MaterialInstance, MaxReferencers));
        }
        return ResultObj;
    }

    FMaterialGraphTarget Target;
    FString ErrorMessage;
    if (!ResolveMaterialGraph(MaterialPath, GraphType, Target, nullptr, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    ResultObj->SetStringField(TEXT("asset_path"), Target.GetPathName());
    ResultObj->SetStringField(TEXT("asset_class"), Target.Asset ? Target.Asset->GetClass()->GetName() : FString());
    ResultObj->SetStringField(TEXT("asset_kind"), Target.IsFunctionGraph() ? TEXT("material_function") : TEXT("material"));
    ResultObj->SetObjectField(TEXT("graph"), GraphSummaryToJson(Target, bIncludeNodes));

    if (Target.Material)
    {
        ResultObj->SetObjectField(TEXT("material_settings"), MaterialSettingsToJson(Target.Material, Target.Material));
        ResultObj->SetArrayField(TEXT("semantic_properties"), SemanticPropertiesToJson(Target.Material, Target.Material));
    }
    else if (Target.Function)
    {
        ResultObj->SetStringField(TEXT("function_path"), Target.Function->GetPathName());
        ResultObj->SetStringField(TEXT("function_name"), Target.Function->GetName());
        ResultObj->SetStringField(TEXT("semantic_note"), TEXT("Material Function has no root shading model or domain; inspect callers for final property semantics."));
    }

    if (bIncludeUsage)
    {
        ResultObj->SetObjectField(TEXT("usage_hints"), UsageHintsToJson(Target.Asset, MaxReferencers));
    }

    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleAddMaterialNode(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material_path' parameter"));
    }

    FString ExpressionClassName;
    if (!Params->TryGetStringField(TEXT("expression_class"), ExpressionClassName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'expression_class' parameter"));
    }

    FString GraphType;
    Params->TryGetStringField(TEXT("graph_type"), GraphType);

    FMaterialGraphTarget Target;
    FString ErrorMessage;
    if (!ResolveMaterialGraph(MaterialPath, GraphType, Target, nullptr, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    UClass* ExpressionClass = FindMaterialExpressionClass(ExpressionClassName);
    if (!ExpressionClass)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material expression class not found: %s"), *ExpressionClassName));
    }

    FVector2D NodePosition(0.0, 0.0);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UObject* SelectedAsset = nullptr;
    FString SelectedAssetPath;
    if (Params->TryGetStringField(TEXT("selected_asset"), SelectedAssetPath) && !SelectedAssetPath.IsEmpty())
    {
        SelectedAsset = LoadObjectValue(SelectedAssetPath);
        if (!SelectedAsset)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Selected asset not found: %s"), *SelectedAssetPath));
        }
    }

    UMaterialExpression* Expression = UMaterialEditingLibrary::CreateMaterialExpressionEx(
        Target.Material,
        Target.Function,
        ExpressionClass,
        SelectedAsset,
        static_cast<int32>(NodePosition.X),
        static_cast<int32>(NodePosition.Y));

    if (!Expression)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to create material expression: %s"), *ExpressionClassName));
    }

    const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
    if (Params->TryGetObjectField(TEXT("properties"), PropertiesObject))
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropertiesObject)->Values)
        {
            if (!SetObjectPropertyValue(Expression, Pair.Key, Pair.Value, ErrorMessage))
            {
                if (Target.Material)
                {
                    UMaterialEditingLibrary::DeleteMaterialExpression(Target.Material, Expression);
                }
                else if (Target.Function)
                {
                    UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Target.Function, Expression);
                }
                return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
            }
        }
    }

    const bool bDeferImmediateUpdate = ShouldDeferImmediateMaterialExpressionUpdate(Expression);
    if (!bDeferImmediateUpdate && !Target.HasUnconnectedCustomInputs())
    {
        RefreshExpressionNodeSafe(Expression);
    }
    Target.MarkChanged(!bDeferImmediateUpdate);

    TSharedPtr<FJsonObject> ResultObj = ExpressionToJson(Expression, true, Target.GetExpressions());
    ResultObj->SetStringField(TEXT("material"), Target.GetPathName());
    ResultObj->SetStringField(TEXT("graph_type"), Target.GetGraphType());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleAddCustomMaterialNode(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material_path' parameter"));
    }

    FString Code;
    if (!Params->TryGetStringField(TEXT("code"), Code))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'code' parameter"));
    }
    if (Code.TrimStartAndEnd().IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Custom material code cannot be empty"));
    }

    FString OutputTypeText = TEXT("CMOT_Float3");
    Params->TryGetStringField(TEXT("output_type"), OutputTypeText);

    ECustomMaterialOutputType OutputType = CMOT_Float3;
    FString ErrorMessage;
    if (!ParseCustomOutputType(OutputTypeText, OutputType, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    TArray<FName> InputNames;
    if (!ParseCustomInputNames(Params, InputNames, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    TArray<FCustomDefine> AdditionalDefines;
    if (!ParseCustomDefines(Params, AdditionalDefines, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    TArray<FString> IncludeFilePaths;
    if (!ParseIncludeFilePaths(Params, IncludeFilePaths, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    FString GraphType;
    Params->TryGetStringField(TEXT("graph_type"), GraphType);

    FMaterialGraphTarget Target;
    if (!ResolveMaterialGraph(MaterialPath, GraphType, Target, nullptr, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    FVector2D NodePosition(0.0, 0.0);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UMaterialExpression* Expression = UMaterialEditingLibrary::CreateMaterialExpressionEx(
        Target.Material,
        Target.Function,
        UMaterialExpressionCustom::StaticClass(),
        nullptr,
        static_cast<int32>(NodePosition.X),
        static_cast<int32>(NodePosition.Y));

    UMaterialExpressionCustom* CustomExpression = Cast<UMaterialExpressionCustom>(Expression);
    if (!CustomExpression)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create MaterialExpressionCustom"));
    }

    FString Description;
    Params->TryGetStringField(TEXT("description"), Description);

    CustomExpression->Modify();
    CustomExpression->Code = Code;
    CustomExpression->OutputType = OutputType;
    CustomExpression->Description = Description;
    CustomExpression->Desc = Description;
    CustomExpression->Inputs.Reset();
    for (const FName& InputName : InputNames)
    {
        FCustomInput CustomInput;
        CustomInput.InputName = InputName;
        CustomExpression->Inputs.Add(CustomInput);
    }
    CustomExpression->AdditionalDefines = AdditionalDefines;
    CustomExpression->IncludeFilePaths = IncludeFilePaths;

    TSharedPtr<FJsonObject> ResultObj = ExpressionToJson(CustomExpression, true, Target.GetExpressions());
    ResultObj->SetStringField(TEXT("material"), Target.GetPathName());
    ResultObj->SetStringField(TEXT("graph_type"), Target.GetGraphType());
    ResultObj->SetBoolField(TEXT("created_custom_node"), true);
    ResultObj->SetNumberField(TEXT("input_count"), InputNames.Num());
    ResultObj->SetStringField(TEXT("requested_output_type"), OutputTypeText);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleSetMaterialNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    FString NodeId;
    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material_path' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'node_id' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_name' parameter"));
    }

    TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
    if (!Value.IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'value' parameter"));
    }

    FString GraphType;
    Params->TryGetStringField(TEXT("graph_type"), GraphType);

    FMaterialGraphTarget Target;
    FString ErrorMessage;
    if (!ResolveMaterialGraph(MaterialPath, GraphType, Target, nullptr, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    UMaterialExpression* Expression = nullptr;
    if (!FindExpressionById(Target, NodeId, Expression, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    if (UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
        FunctionCall && PropertyName.Equals(TEXT("MaterialFunction"), ESearchCase::IgnoreCase))
    {
        if (!Value.IsValid() || Value->Type != EJson::String)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("MaterialFunction property requires a Material Function asset path string"));
        }

        UMaterialFunctionInterface* NewFunction = nullptr;
        if (!Value->AsString().IsEmpty())
        {
            NewFunction = Cast<UMaterialFunctionInterface>(LoadObjectValue(Value->AsString()));
            if (!NewFunction)
            {
                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material Function not found: %s"), *Value->AsString()));
            }
        }

        FunctionCall->Modify();
        if (!FunctionCall->SetMaterialFunction(NewFunction))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to set MaterialFunction on node: %s"), *NodeId));
        }
        FunctionCall->UpdateFromFunctionResource(true);
    }
    else if (!SetObjectPropertyValue(Expression, PropertyName, Value, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    if (!Target.HasUnconnectedCustomInputs())
    {
        RefreshExpressionNodeSafe(Expression);
    }
    Target.MarkChanged(Cast<UMaterialExpressionCustom>(Expression) == nullptr);

    TSharedPtr<FJsonObject> ResultObj = ExpressionToJson(Expression, true, Target.GetExpressions());
    ResultObj->SetStringField(TEXT("updated_property"), PropertyName);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleConnectMaterialNodes(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    FString FromNodeId;
    FString ToNodeId;
    FString ToInput;
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material_path' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("from_node_id"), FromNodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'from_node_id' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("to_node_id"), ToNodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'to_node_id' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("to_input"), ToInput))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'to_input' parameter"));
    }

    FString FromOutput;
    Params->TryGetStringField(TEXT("from_output"), FromOutput);

    FString GraphType;
    Params->TryGetStringField(TEXT("graph_type"), GraphType);

    FMaterialGraphTarget Target;
    FString ErrorMessage;
    if (!ResolveMaterialGraph(MaterialPath, GraphType, Target, nullptr, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    UMaterialExpression* FromExpression = nullptr;
    UMaterialExpression* ToExpression = nullptr;
    if (!FindExpressionById(Target, FromNodeId, FromExpression, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }
    if (!FindExpressionById(Target, ToNodeId, ToExpression, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    if (FindOutputIndexByName(FromExpression, FromOutput) == INDEX_NONE)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Source output not found: '%s'. Available outputs: %s"),
            *FromOutput,
            *FString::Join(GetMaterialExpressionOutputNames(FromExpression), TEXT(", "))));
    }
    if (!HasInputName(ToExpression, ToInput))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Target input not found: '%s'. Available inputs: %s"),
            *ToInput,
            *FString::Join(GetMaterialExpressionInputNamesSafe(ToExpression), TEXT(", "))));
    }

    const FString CanonicalFromOutput = ResolveOutputConnectName(FromExpression, FromOutput);
    const FString CanonicalToInput = ResolveInputConnectName(ToExpression, ToInput);
    const bool bConnected = UMaterialEditingLibrary::ConnectMaterialExpressions(FromExpression, CanonicalFromOutput, ToExpression, CanonicalToInput);
    if (!bConnected)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to connect material nodes: %s.%s -> %s.%s"),
            *FromNodeId, *CanonicalFromOutput, *ToNodeId, *CanonicalToInput));
    }

    const bool bTouchesCustomExpression = Cast<UMaterialExpressionCustom>(FromExpression) != nullptr ||
        Cast<UMaterialExpressionCustom>(ToExpression) != nullptr;
    Target.MarkChanged(!bTouchesCustomExpression);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("material"), Target.GetPathName());
    ResultObj->SetStringField(TEXT("graph_type"), Target.GetGraphType());
    ResultObj->SetStringField(TEXT("from_node_id"), GetExpressionId(FromExpression));
    ResultObj->SetStringField(TEXT("from_output"), CanonicalFromOutput);
    ResultObj->SetStringField(TEXT("to_node_id"), GetExpressionId(ToExpression));
    ResultObj->SetStringField(TEXT("to_input"), CanonicalToInput);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleConnectMaterialProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    FString FromNodeId;
    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material_path' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("from_node_id"), FromNodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'from_node_id' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("property"), PropertyName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property' parameter"));
    }

    FString FromOutput;
    Params->TryGetStringField(TEXT("from_output"), FromOutput);

    FMaterialGraphTarget Target;
    FString ErrorMessage;
    if (!ResolveMaterialGraph(MaterialPath, TEXT("material"), Target, nullptr, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }
    if (!Target.Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("connect_material_property only supports Material graphs, not Material Functions"));
    }

    UMaterialExpression* FromExpression = nullptr;
    if (!FindExpressionById(Target, FromNodeId, FromExpression, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    EMaterialProperty MaterialProperty = MP_MAX;
    if (!MaterialPropertyFromString(PropertyName, MaterialProperty, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    if (!Target.Material->GetExpressionInputForProperty(MaterialProperty))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material property cannot be connected in this material: %s"), *PropertyName));
    }
    if (FindOutputIndexByName(FromExpression, FromOutput) == INDEX_NONE)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Source output not found: '%s'. Available outputs: %s"),
            *FromOutput,
            *FString::Join(GetMaterialExpressionOutputNames(FromExpression), TEXT(", "))));
    }

    const FString CanonicalFromOutput = ResolveOutputConnectName(FromExpression, FromOutput);
    const bool bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(FromExpression, CanonicalFromOutput, MaterialProperty);
    if (!bConnected)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to connect material property: %s.%s -> %s"),
            *FromNodeId, *CanonicalFromOutput, *PropertyName));
    }

    Target.MarkChanged(Cast<UMaterialExpressionCustom>(FromExpression) == nullptr);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("material"), Target.GetPathName());
    ResultObj->SetStringField(TEXT("from_node_id"), GetExpressionId(FromExpression));
    ResultObj->SetStringField(TEXT("from_output"), CanonicalFromOutput);
    ResultObj->SetObjectField(TEXT("property_connection"), MaterialPropertyConnectionToJson(Target.Material, MaterialProperty));
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleDeleteMaterialNode(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    FString NodeId;
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material_path' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'node_id' parameter"));
    }

    FString GraphType;
    Params->TryGetStringField(TEXT("graph_type"), GraphType);

    FMaterialGraphTarget Target;
    FString ErrorMessage;
    if (!ResolveMaterialGraph(MaterialPath, GraphType, Target, nullptr, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    UMaterialExpression* Expression = nullptr;
    if (!FindExpressionById(Target, NodeId, Expression, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    const FString DeletedNodeId = GetExpressionId(Expression);
    if (Target.Material)
    {
        UMaterialEditingLibrary::DeleteMaterialExpression(Target.Material, Expression);
    }
    else if (Target.Function)
    {
        UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Target.Function, Expression);
    }

    Target.MarkChanged();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("material"), Target.GetPathName());
    ResultObj->SetStringField(TEXT("graph_type"), Target.GetGraphType());
    ResultObj->SetStringField(TEXT("deleted_node_id"), DeletedNodeId);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleLayoutMaterialNodes(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material_path' parameter"));
    }

    FString GraphType;
    Params->TryGetStringField(TEXT("graph_type"), GraphType);

    FMaterialGraphTarget Target;
    FString ErrorMessage;
    if (!ResolveMaterialGraph(MaterialPath, GraphType, Target, nullptr, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    if (Target.Material)
    {
        UMaterialEditingLibrary::LayoutMaterialExpressions(Target.Material);
    }
    else if (Target.Function)
    {
        UMaterialEditingLibrary::LayoutMaterialFunctionExpressions(Target.Function);
    }

    Target.MarkChanged();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("material"), Target.GetPathName());
    ResultObj->SetStringField(TEXT("graph_type"), Target.GetGraphType());
    ResultObj->SetBoolField(TEXT("laid_out"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleCompileAndSaveMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material_path' parameter"));
    }

    FString GraphType;
    Params->TryGetStringField(TEXT("graph_type"), GraphType);

    bool bSave = true;
    if (Params->HasField(TEXT("save")))
    {
        bSave = Params->GetBoolField(TEXT("save"));
    }

    FMaterialGraphTarget Target;
    FString ErrorMessage;
    if (!ResolveMaterialGraph(MaterialPath, GraphType, Target, nullptr, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    UPackage* Package = Target.Asset ? Target.Asset->GetOutermost() : nullptr;
    const bool bWasDirty = Package ? Package->IsDirty() : false;
    TArray<FString> CompileErrors;
    bool bCompilationFinished = true;
    bool bCompiled = true;

    if (Target.Material)
    {
        UMaterialEditingLibrary::RecompileMaterial(Target.Material);
        FMaterialResource* MaterialResource = Target.Material->GetMaterialResource(GMaxRHIShaderPlatform);
        if (MaterialResource)
        {
            MaterialResource->FinishCompilation();
            bCompilationFinished = MaterialResource->IsCompilationFinished();
            CompileErrors = MaterialResource->GetCompileErrors();
            bCompiled = bCompilationFinished && CompileErrors.Num() == 0 && !Target.Material->IsCompilingOrHadCompileError(GMaxRHIShaderPlatform);
        }
        else
        {
            bCompiled = false;
            CompileErrors.Add(TEXT("No material resource is available for the current shader platform."));
        }
    }
    else if (Target.Function)
    {
        UMaterialEditingLibrary::UpdateMaterialFunction(Target.Function, nullptr);
    }

    bool bSaved = false;
    if (bSave)
    {
        Target.MarkChanged();
        bSaved = UEditorAssetLibrary::SaveLoadedAsset(Target.Asset, false);
    }
    else if (Package && !bWasDirty && Package->IsDirty())
    {
        Package->SetDirtyFlag(false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("material"), Target.GetPathName());
    ResultObj->SetStringField(TEXT("graph_type"), Target.GetGraphType());
    ResultObj->SetBoolField(TEXT("compiled"), bCompiled);
    ResultObj->SetBoolField(TEXT("compilation_finished"), bCompilationFinished);
    ResultObj->SetNumberField(TEXT("compile_error_count"), CompileErrors.Num());
    ResultObj->SetArrayField(TEXT("compile_errors"), StringArrayToJson(CompileErrors));
    ResultObj->SetBoolField(TEXT("saved"), bSaved);
    ResultObj->SetBoolField(TEXT("was_dirty_before_compile"), bWasDirty);
    ResultObj->SetBoolField(TEXT("dirty_after_compile"), Package ? Package->IsDirty() : false);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleExpandMaterialFunctionCalls(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material_path' parameter"));
    }

    FString NodeId;
    Params->TryGetStringField(TEXT("node_id"), NodeId);
    NodeId.TrimStartAndEndInline();

    bool bRecursive = true;
    if (Params->HasField(TEXT("recursive")))
    {
        bRecursive = Params->GetBoolField(TEXT("recursive"));
    }

    bool bExcludeEngineFunctions = true;
    if (Params->HasField(TEXT("exclude_engine_functions")))
    {
        bExcludeEngineFunctions = Params->GetBoolField(TEXT("exclude_engine_functions"));
    }

    bool bSave = true;
    if (Params->HasField(TEXT("save")))
    {
        bSave = Params->GetBoolField(TEXT("save"));
    }

    bool bAllowPartialSave = false;
    if (Params->HasField(TEXT("allow_partial_save")))
    {
        bAllowPartialSave = Params->GetBoolField(TEXT("allow_partial_save"));
    }

    int32 MaxPasses = bRecursive ? 8 : 1;
    double MaxPassesValue = 0.0;
    if (Params->TryGetNumberField(TEXT("max_passes"), MaxPassesValue))
    {
        MaxPasses = FMath::Clamp(static_cast<int32>(MaxPassesValue), 1, 64);
    }

    FMaterialGraphTarget Target;
    FString ErrorMessage;
    if (!ResolveMaterialGraph(MaterialPath, TEXT("material"), Target, nullptr, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    if (!Target.Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("expand_material_function_calls currently supports Material assets only"));
    }

    UPackage* Package = Target.Asset ? Target.Asset->GetOutermost() : nullptr;
    const bool bWasDirty = Package ? Package->IsDirty() : false;
    const int32 InitialNodeCount = Target.Material->GetExpressions().Num();
    const int32 InitialFunctionCallCount = CountMaterialFunctionCalls(Target.Material);

    TArray<TSharedPtr<FJsonValue>> Expanded;
    TArray<TSharedPtr<FJsonValue>> Skipped;
    TArray<FString> Errors;
    TSet<FString> FailedExpansionNodeIds;
    int32 ExpandedCount = 0;
    int32 PassesRun = 0;
    bool bHitMaxPasses = false;
    const bool bTargetedExpansion = !NodeId.IsEmpty();
    TArray<FString> PendingTargetNodeIds;
    if (bTargetedExpansion)
    {
        PendingTargetNodeIds.Add(NodeId);
    }

    for (int32 PassIndex = 0; PassIndex < MaxPasses; ++PassIndex)
    {
        ++PassesRun;

        TArray<FString> NodeIds;
        if (bTargetedExpansion)
        {
            NodeIds = MoveTemp(PendingTargetNodeIds);
        }
        else
        {
            NodeIds = GetExpandableFunctionCallNodeIds(Target.Material, bExcludeEngineFunctions, Skipped);
        }

        if (NodeIds.Num() == 0)
        {
            break;
        }

        int32 ExpandedThisPass = 0;
        TArray<FString> NextTargetNodeIds;
        for (const FString& CurrentNodeId : NodeIds)
        {
            if (FailedExpansionNodeIds.Contains(CurrentNodeId))
            {
                continue;
            }

            UMaterialExpression* Expression = nullptr;
            FString FindError;
            if (!FindExpressionById(Target, CurrentNodeId, Expression, FindError))
            {
                Errors.Add(FindError);
                FailedExpansionNodeIds.Add(CurrentNodeId);
                continue;
            }

            UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
            if (!FunctionCall)
            {
                TSharedPtr<FJsonObject> SkippedObject = MakeShared<FJsonObject>();
                AddExpressionReferenceFields(SkippedObject, Expression, TEXT(""));
                SkippedObject->SetStringField(TEXT("reason"), TEXT("not_a_material_function_call"));
                Skipped.Add(MakeShared<FJsonValueObject>(SkippedObject));
                continue;
            }

            UMaterialFunctionInterface* MaterialFunction = FunctionCall->MaterialFunction;
            const FString FunctionPath = MaterialFunction ? MaterialFunction->GetPathName() : FString();
            if (!MaterialFunction)
            {
                TSharedPtr<FJsonObject> SkippedObject = MakeShared<FJsonObject>();
                AddExpressionReferenceFields(SkippedObject, FunctionCall, TEXT(""));
                SkippedObject->SetStringField(TEXT("reason"), TEXT("missing_material_function"));
                Skipped.Add(MakeShared<FJsonValueObject>(SkippedObject));
                continue;
            }

            if (bExcludeEngineFunctions && IsEngineOrScriptAssetPath(FunctionPath))
            {
                TSharedPtr<FJsonObject> SkippedObject = MakeShared<FJsonObject>();
                AddExpressionReferenceFields(SkippedObject, FunctionCall, TEXT(""));
                SkippedObject->SetStringField(TEXT("function"), FunctionPath);
                SkippedObject->SetStringField(TEXT("reason"), TEXT("engine_or_script_function"));
                Skipped.Add(MakeShared<FJsonValueObject>(SkippedObject));
                continue;
            }

            const int32 BeforeNodeCount = Target.Material->GetExpressions().Num();
            FMaterialFunctionExpansionStats ExpansionStats;
            if (!ExpandFunctionCallIntoMaterial(Target.Material, FunctionCall, ExpansionStats, Errors))
            {
                FailedExpansionNodeIds.Add(CurrentNodeId);
                continue;
            }
            const int32 AfterNodeCount = Target.Material->GetExpressions().Num();

            TSharedPtr<FJsonObject> ExpandedObject = MakeShared<FJsonObject>();
            ExpandedObject->SetNumberField(TEXT("pass"), PassIndex + 1);
            ExpandedObject->SetStringField(TEXT("requested_node_id"), CurrentNodeId);
            ExpandedObject->SetStringField(TEXT("function"), FunctionPath);
            ExpandedObject->SetNumberField(TEXT("node_count_before"), BeforeNodeCount);
            ExpandedObject->SetNumberField(TEXT("node_count_after"), AfterNodeCount);
            ExpandedObject->SetNumberField(TEXT("duplicated_node_count"), ExpansionStats.DuplicatedNodeCount);
            ExpandedObject->SetNumberField(TEXT("preview_default_node_count"), ExpansionStats.PreviewDefaultNodeCount);
            ExpandedObject->SetNumberField(TEXT("rewired_function_input_count"), ExpansionStats.RewiredFunctionInputCount);
            ExpandedObject->SetNumberField(TEXT("rewired_consumer_count"), ExpansionStats.RewiredConsumerCount);
            ExpandedObject->SetArrayField(TEXT("created_function_call_node_ids"), StringArrayToJson(ExpansionStats.CreatedFunctionCallNodeIds));
            Expanded.Add(MakeShared<FJsonValueObject>(ExpandedObject));

            if (bTargetedExpansion && bRecursive)
            {
                NextTargetNodeIds.Append(ExpansionStats.CreatedFunctionCallNodeIds);
            }

            ++ExpandedCount;
            ++ExpandedThisPass;
        }

        if (!bRecursive || ExpandedThisPass == 0)
        {
            break;
        }

        if (bTargetedExpansion)
        {
            PendingTargetNodeIds = MoveTemp(NextTargetNodeIds);
            if (PendingTargetNodeIds.Num() == 0)
            {
                break;
            }
        }

        if (PassIndex + 1 == MaxPasses)
        {
            if (bTargetedExpansion)
            {
                bHitMaxPasses = PendingTargetNodeIds.Num() > 0;
            }
            else
            {
                TArray<TSharedPtr<FJsonValue>> RemainingSkipped;
                bHitMaxPasses = GetExpandableFunctionCallNodeIds(Target.Material, bExcludeEngineFunctions, RemainingSkipped).Num() > 0;
            }
        }
    }

    bool bSaved = false;
    if (ExpandedCount > 0)
    {
        Target.MarkChanged();
        if (bSave && (Errors.Num() == 0 || bAllowPartialSave))
        {
            bSaved = UEditorAssetLibrary::SaveLoadedAsset(Target.Asset, false);
        }
    }
    else if (Package && !bWasDirty && Package->IsDirty())
    {
        Package->SetDirtyFlag(false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("material"), Target.GetPathName());
    ResultObj->SetBoolField(TEXT("expanded"), ExpandedCount > 0);
    ResultObj->SetNumberField(TEXT("expanded_count"), ExpandedCount);
    ResultObj->SetNumberField(TEXT("passes_run"), PassesRun);
    ResultObj->SetBoolField(TEXT("hit_max_passes"), bHitMaxPasses);
    ResultObj->SetBoolField(TEXT("exclude_engine_functions"), bExcludeEngineFunctions);
    ResultObj->SetBoolField(TEXT("allow_partial_save"), bAllowPartialSave);
    ResultObj->SetBoolField(TEXT("partial_expansion_with_errors"), ExpandedCount > 0 && Errors.Num() > 0);
    ResultObj->SetNumberField(TEXT("initial_node_count"), InitialNodeCount);
    ResultObj->SetNumberField(TEXT("final_node_count"), Target.Material->GetExpressions().Num());
    ResultObj->SetNumberField(TEXT("initial_function_call_count"), InitialFunctionCallCount);
    ResultObj->SetNumberField(TEXT("final_function_call_count"), CountMaterialFunctionCalls(Target.Material));
    ResultObj->SetArrayField(TEXT("expanded_nodes"), Expanded);
    ResultObj->SetArrayField(TEXT("skipped_nodes"), Skipped);
    ResultObj->SetArrayField(TEXT("errors"), StringArrayToJson(Errors));
    ResultObj->SetBoolField(TEXT("saved"), bSaved);
    ResultObj->SetBoolField(TEXT("was_dirty_before_expand"), bWasDirty);
    ResultObj->SetBoolField(TEXT("dirty_after_expand"), Package ? Package->IsDirty() : false);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleGetMaterialParameterCollectionValues(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionPath;
    if (!Params->TryGetStringField(TEXT("collection_path"), CollectionPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'collection_path' parameter"));
    }

    bool bIncludeAssetDefaults = true;
    if (Params->HasField(TEXT("include_asset_defaults")))
    {
        bIncludeAssetDefaults = Params->GetBoolField(TEXT("include_asset_defaults"));
    }

    bool bIncludeRuntime = true;
    if (Params->HasField(TEXT("include_runtime")))
    {
        bIncludeRuntime = Params->GetBoolField(TEXT("include_runtime"));
    }

    UMaterialParameterCollection* Collection = Cast<UMaterialParameterCollection>(LoadObjectValue(CollectionPath));
    if (!Collection)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material Parameter Collection not found: %s"), *CollectionPath));
    }

    UWorld* World = bIncludeRuntime ? GetEditorOrPIEWorld() : nullptr;
    UMaterialParameterCollectionInstance* Instance = (World && bIncludeRuntime) ? World->GetParameterCollectionInstance(Collection) : nullptr;
    const TSet<FName> ParameterFilter = GetOptionalParameterNameFilter(Params);

    TArray<TSharedPtr<FJsonValue>> ScalarValues;
    for (const FCollectionScalarParameter& Parameter : Collection->ScalarParameters)
    {
        if (!ShouldIncludeNamedParameter(ParameterFilter, Parameter.ParameterName))
        {
            continue;
        }

        TSharedPtr<FJsonObject> ParameterObject = MakeShared<FJsonObject>();
        ParameterObject->SetStringField(TEXT("name"), Parameter.ParameterName.ToString());
        ParameterObject->SetStringField(TEXT("id"), Parameter.Id.ToString(EGuidFormats::DigitsWithHyphens));

        if (bIncludeAssetDefaults)
        {
            ParameterObject->SetNumberField(TEXT("asset_default"), Parameter.DefaultValue);
        }

        bool bRuntimeResolved = false;
        float RuntimeValue = 0.0f;
        if (Instance)
        {
            bRuntimeResolved = Instance->GetScalarParameterValue(Parameter.ParameterName, RuntimeValue);
        }
        ParameterObject->SetBoolField(TEXT("runtime_resolved"), bRuntimeResolved);
        if (bRuntimeResolved)
        {
            ParameterObject->SetNumberField(TEXT("runtime_value"), RuntimeValue);
        }

        ScalarValues.Add(MakeShared<FJsonValueObject>(ParameterObject));
    }

    TArray<TSharedPtr<FJsonValue>> VectorValues;
    for (const FCollectionVectorParameter& Parameter : Collection->VectorParameters)
    {
        if (!ShouldIncludeNamedParameter(ParameterFilter, Parameter.ParameterName))
        {
            continue;
        }

        TSharedPtr<FJsonObject> ParameterObject = MakeShared<FJsonObject>();
        ParameterObject->SetStringField(TEXT("name"), Parameter.ParameterName.ToString());
        ParameterObject->SetStringField(TEXT("id"), Parameter.Id.ToString(EGuidFormats::DigitsWithHyphens));

        if (bIncludeAssetDefaults)
        {
            ParameterObject->SetObjectField(TEXT("asset_default"), LinearColorToJson(Parameter.DefaultValue));
        }

        bool bRuntimeResolved = false;
        FLinearColor RuntimeValue = FLinearColor::Transparent;
        if (Instance)
        {
            bRuntimeResolved = Instance->GetVectorParameterValue(Parameter.ParameterName, RuntimeValue);
        }
        ParameterObject->SetBoolField(TEXT("runtime_resolved"), bRuntimeResolved);
        if (bRuntimeResolved)
        {
            ParameterObject->SetObjectField(TEXT("runtime_value"), LinearColorToJson(RuntimeValue));
        }

        VectorValues.Add(MakeShared<FJsonValueObject>(ParameterObject));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("collection"), Collection->GetPathName());
    ResultObj->SetBoolField(TEXT("include_asset_defaults"), bIncludeAssetDefaults);
    ResultObj->SetBoolField(TEXT("include_runtime"), bIncludeRuntime);
    ResultObj->SetBoolField(TEXT("has_world"), World != nullptr);
    ResultObj->SetStringField(TEXT("world_type"), GetWorldTypeName(World));
    ResultObj->SetBoolField(TEXT("has_runtime_instance"), Instance != nullptr);
    ResultObj->SetArrayField(TEXT("scalars"), ScalarValues);
    ResultObj->SetArrayField(TEXT("vectors"), VectorValues);
    ResultObj->SetNumberField(TEXT("scalar_count"), ScalarValues.Num());
    ResultObj->SetNumberField(TEXT("vector_count"), VectorValues.Num());
    return ResultObj;
}
