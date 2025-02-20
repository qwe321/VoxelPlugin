// Copyright 2021 Phyronnaz

#include "VoxelConvertLandscapeMaterial.h"

#include "AssetRegistryModule.h"
#include "VoxelMinimal.h"
#include "VoxelFoliage.h"
#include "VoxelFoliageCollection.h"
#include "VoxelRender/VoxelMaterialExpressions.h"
#include "VoxelRender/MaterialCollections/VoxelLandscapeMaterialCollection.h"
#include "VoxelEditorUtilities.h"

#include "Editor.h"
#include "Materials/Material.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "KismetCompilerModule.h"
#include "ScopedTransaction.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/MessageDialog.h"
#include "Subsystems/AssetEditorSubsystem.h"

void FVoxelConvertLandscapeMaterial::Init()
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	ContentBrowserModule.GetAllAssetViewContextMenuExtenders().Add(FContentBrowserMenuExtender_SelectedAssets::CreateLambda([=](const TArray<FAssetData>& SelectedAssets)
	{
		const auto Extender = MakeShared<FExtender>();

		for (auto& Asset : SelectedAssets)
		{
			if (Asset.GetClass() != UMaterial::StaticClass())
			{
				return Extender;
			}
		}
		
		Extender->AddMenuExtension(
			"CommonAssetActions",
			EExtensionHook::After,
			nullptr,
			FMenuExtensionDelegate::CreateLambda([=](FMenuBuilder& MenuBuilder)
			{
				MenuBuilder.AddMenuEntry(
				VOXEL_LOCTEXT("Convert landscape material to voxel"),
				VOXEL_LOCTEXT("Will replace all landscape layer nodes with nodes compatible with both voxels and landscapes"),
				FSlateIcon(NAME_None, NAME_None),
				FUIAction(FExecuteAction::CreateLambda([=]()
				{
					for (auto& Asset : SelectedAssets)
					{
						auto* Material = Cast<UMaterial>(Asset.GetAsset());
						if (ensure(Material))
						{
							ConvertMaterial(Material);
						}
					}
				})));
			}));

		return Extender;
	}));

}

void FVoxelConvertLandscapeMaterial::ConvertMaterial(UMaterial* Material)
{
	FScopedTransaction Transaction(TEXT("ConvertMaterial"), VOXEL_LOCTEXT("Convert landscape material to voxel"), Material);

	TSet<UMaterialFunction*> VisitedFunctions;
	const int32 NumReplaced = ConvertExpressions(Material, Material->GetExpressions(), VisitedFunctions);

	const FText Text = FText::Format(VOXEL_LOCTEXT("{0} expressions replaced in {1}"), NumReplaced, FText::FromName(Material->GetFName()));
	LOG_VOXEL(Log, TEXT("%s"), *Text.ToString());

	FNotificationInfo Info(Text);
	Info.ExpireDuration = 10.f;
	Info.CheckBoxState = ECheckBoxState::Checked;
	FSlateNotificationManager::Get().AddNotification(Info);
}

int32 FVoxelConvertLandscapeMaterial::ConvertExpressions(UObject* Owner, const TConstArrayView<TObjectPtr<UMaterialExpression>>& Expressions, TSet<UMaterialFunction*>& VisitedFunctions)
{
	int32 NumReplaced = 0;
	
	const auto ExpressionsCopy = Expressions;
	for (auto Expression : ExpressionsCopy)
	{
		auto* VoxelClass = FVoxelMaterialExpressionUtilities::GetVoxelExpression(Expression->GetClass());
		if (VoxelClass)
		{
			ConvertExpression(Owner, Expression, VoxelClass);
			NumReplaced++;
		}
		if (auto* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
		{
			auto* Function = Cast<UMaterialFunction>(FunctionCall->MaterialFunction);
			if (Function && !VisitedFunctions.Contains(Function))
			{
				VisitedFunctions.Add(Function);
				NumReplaced += ConvertExpressions(Function, Function->GetExpressions(), VisitedFunctions);
			}
		}
	}
	
	return NumReplaced;
}

void FVoxelConvertLandscapeMaterial::ConvertExpression(UObject* Owner, UMaterialExpression* Expression, UClass* NewClass)
{
	Owner->Modify();
	
	auto* NewExpression = NewObject<UMaterialExpression>(Owner, NewClass, NAME_None, RF_Transactional);
	check(NewClass->IsChildOf(Expression->GetClass()));

	Expression->Modify();
	NewExpression->Modify();

	const auto& ExpressionsView = Owner->IsA<UMaterial>() ? CastChecked<UMaterial>(Owner)->GetExpressions(): CastChecked<UMaterialFunction>(Owner)->GetExpressions();
	TArray<TObjectPtr<UMaterialExpression>> Expressions;
	for (auto Expr : ExpressionsView)
		Expressions.Add(Expr);
	
	ensure(Expressions.Remove(Expression) == 1);
	Expressions.Add(NewExpression);

	// Copy data
	for (TFieldIterator<FProperty> It(Expression->GetClass()); It; ++It)
	{
		auto* Property = *It;
		if (!Property->HasAnyPropertyFlags(CPF_Transient))
		{
			Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(NewExpression), Property->ContainerPtrToValuePtr<void>(Expression));
		}
	}
	
	// Fixup other links
	for (UMaterialExpression* OtherExpression : Expressions)
	{
		if (OtherExpression != Expression)
		{
			for (FExpressionInput* Input : OtherExpression->GetInputs())
			{
				if (Input->Expression == Expression)
				{
					OtherExpression->Modify();
					Input->Expression = NewExpression;
				}
			}
		}
	}
	
	// Check material parameter inputs
	if (auto* Material = Cast<UMaterial>(Owner))
	{
		for (int32 InputIndex = 0; InputIndex < MP_MAX; InputIndex++)
		{
			FExpressionInput* Input = Material->GetExpressionInputForProperty((EMaterialProperty)InputIndex);
			if (Input && Input->Expression == Expression)
			{
				Input->Expression = NewExpression;
			}
		}
	}
}