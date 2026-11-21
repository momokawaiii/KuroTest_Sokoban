#if WITH_EDITOR
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    TSharedPtr<FJsonObject> DescribeField(FProperty* P)
    {
        auto D=MakeShared<FJsonObject>();
        D->SetStringField(TEXT("type"),P->GetClass()->GetName());
        if(auto* S=CastField<FStructProperty>(P)) D->SetStringField(TEXT("struct"),S->Struct->GetName());
        if(auto* A=CastField<FArrayProperty>(P)) D->SetObjectField(TEXT("inner"),DescribeField(A->Inner));
        if(auto* E=CastField<FEnumProperty>(P)) D->SetObjectField(TEXT("underlying"),DescribeField(E->GetUnderlyingProperty()));
        if(auto* A=CastField<FMapProperty>(P))
        {
            D->SetObjectField(TEXT("key"),DescribeField(A->KeyProp));
            D->SetObjectField(TEXT("value"),DescribeField(A->ValueProp));
        }
        if(auto* A=CastField<FSetProperty>(P)) D->SetObjectField(TEXT("inner"),DescribeField(A->ElementProp));
        return D;
    }

    void ExportCookSchema()
    {
        auto All=MakeShared<FJsonObject>();
        for(TObjectIterator<UStruct> It;It;++It)
        {
            if(It->IsA<UFunction>() || !It->GetOutermost()->GetName().StartsWith(TEXT("/Script/"))) continue;
            TArray<TSharedPtr<FJsonValue>> Fields;
            for(FProperty* P=It->PropertyLink;P;P=P->PropertyLinkNext)
            {
                if(P->IsEditorOnlyProperty()) continue;
                auto D=DescribeField(P);
                D->SetStringField(TEXT("name"),P->GetName());
                for(int I=0;I<P->ArrayDim;++I) Fields.Add(MakeShared<FJsonValueObject>(D));
            }
            All->SetArrayField(It->GetName(),Fields);
        }
        FString Out;
        auto Writer=TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
        FJsonSerializer::Serialize(All,Writer);
        FFileHelper::SaveStringToFile(Out,*(FPaths::ProjectSavedDir()/TEXT("NatureTheoryStudy/CookSchema.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }
    FAutoConsoleCommand Cmd(TEXT("RR.ExportCookSchema"),TEXT("Export reflection for offline cooked VFX property comparison."),
        FConsoleCommandDelegate::CreateStatic(&ExportCookSchema));
}
#endif
