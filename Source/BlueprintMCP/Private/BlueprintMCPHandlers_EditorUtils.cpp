#include "BlueprintMCPServer.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "LevelEditorViewport.h"
#include "FileHelpers.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

// ============================================================
// Helper — find an actor by label
// ============================================================

static AActor* FindActorByLabelEditorUtils(const FString& Label, FString& OutError)
{
	if (!GEditor)
	{
		OutError = TEXT("Editor not available.");
		return nullptr;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		OutError = TEXT("No editor world available.");
		return nullptr;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && Actor->GetActorLabel() == Label)
		{
			return Actor;
		}
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && Actor->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase))
		{
			return Actor;
		}
	}

	OutError = FString::Printf(TEXT("Actor with label '%s' not found."), *Label);
	return nullptr;
}

// ============================================================
// HandleFocusActor — focus the viewport on an actor
// ============================================================

FString FBlueprintMCPServer::HandleFocusActor(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body."));
	}

	FString ActorLabel;
	if (!Json->TryGetStringField(TEXT("actorLabel"), ActorLabel) || ActorLabel.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: 'actorLabel'."));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: focus_actor('%s')"), *ActorLabel);

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("focus_actor requires editor mode."));
	}

	FString Error;
	AActor* Actor = FindActorByLabelEditorUtils(ActorLabel, Error);
	if (!Actor) return MakeErrorJson(Error);

	// Select the actor and focus
	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(Actor, true, true);
	GEditor->MoveViewportCamerasToActor(*Actor, false);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actorLabel"), ActorLabel);

	FVector Loc = Actor->GetActorLocation();
	TSharedRef<FJsonObject> LocObj = MakeShared<FJsonObject>();
	LocObj->SetNumberField(TEXT("x"), Loc.X);
	LocObj->SetNumberField(TEXT("y"), Loc.Y);
	LocObj->SetNumberField(TEXT("z"), Loc.Z);
	Result->SetObjectField(TEXT("location"), LocObj);

	return JsonToString(Result);
}

// ============================================================
// HandleEditorNotification — show a toast notification
// ============================================================

FString FBlueprintMCPServer::HandleEditorNotification(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body."));
	}

	FString Message;
	if (!Json->TryGetStringField(TEXT("message"), Message) || Message.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: 'message'."));
	}

	FString SeverityStr;
	Json->TryGetStringField(TEXT("severity"), SeverityStr);

	double Duration = 5.0;
	Json->TryGetNumberField(TEXT("duration"), Duration);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: editor_notification('%s')"), *Message);

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("editor_notification requires editor mode."));
	}

	SNotificationItem::ECompletionState CompletionState = SNotificationItem::CS_None;
	if (SeverityStr.Equals(TEXT("success"), ESearchCase::IgnoreCase))
	{
		CompletionState = SNotificationItem::CS_Success;
	}
	else if (SeverityStr.Equals(TEXT("fail"), ESearchCase::IgnoreCase) || SeverityStr.Equals(TEXT("error"), ESearchCase::IgnoreCase))
	{
		CompletionState = SNotificationItem::CS_Fail;
	}
	else if (SeverityStr.Equals(TEXT("pending"), ESearchCase::IgnoreCase))
	{
		CompletionState = SNotificationItem::CS_Pending;
	}

	FNotificationInfo Info(FText::FromString(Message));
	Info.bFireAndForget = true;
	Info.ExpireDuration = Duration;
	Info.bUseLargeFont = false;

	TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
	if (Notification.IsValid() && CompletionState != SNotificationItem::CS_None)
	{
		Notification->SetCompletionState(CompletionState);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("message"), Message);
	Result->SetNumberField(TEXT("duration"), Duration);

	return JsonToString(Result);
}

// ============================================================
// HandleSaveAll — save all dirty packages
// ============================================================

namespace
{
	// Pick a representative top-level asset inside the package: prefer UWorld for
	// .umap packages, otherwise the first RF_Public|RF_Standalone object. Returns
	// nullptr if the package only contains transient/inner objects (skip those).
	UObject* PickPrimaryAsset(UPackage* Package)
	{
		if (!Package) return nullptr;

		UObject* PrimaryAsset = nullptr;
		UWorld* MaybeWorld = nullptr;
		ForEachObjectWithPackage(Package, [&PrimaryAsset, &MaybeWorld](UObject* Obj)
		{
			if (UWorld* W = Cast<UWorld>(Obj))
			{
				MaybeWorld = W;
				return false; // stop — UWorld wins
			}
			if (!PrimaryAsset && Obj->HasAnyFlags(RF_Public | RF_Standalone) && !Obj->IsA<UPackage>())
			{
				PrimaryAsset = Obj;
			}
			return true;
		});
		return MaybeWorld ? static_cast<UObject*>(MaybeWorld) : PrimaryAsset;
	}

#if PLATFORM_WINDOWS
	// SEH wrapper: UPackage::Save can crash with structured exceptions on rare
	// edge cases (corrupted CDOs, bad replication metadata). Catch the SEH so
	// the MCP server stays alive and we can move on to the next package.
	__declspec(noinline) ESavePackageResult SavePackageInnerEditorUtils(
		UPackage* Package, UObject* Asset, const TCHAR* Filename, FSavePackageArgs* Args)
	{
		return UPackage::Save(Package, Asset, Filename, *Args).Result;
	}

	int32 TrySavePackageEditorUtils(
		UPackage* Package, UObject* Asset, const TCHAR* Filename,
		FSavePackageArgs* Args, ESavePackageResult* OutResult)
	{
		__try
		{
			*OutResult = SavePackageInnerEditorUtils(Package, Asset, Filename, Args);
			return 0;
		}
		__except (1)
		{
			*OutResult = ESavePackageResult::Error;
			return -1;
		}
	}
#endif

	// Save one package with retry on transient sharing violations (Defender,
	// Search Indexer, antivirus). Returns Success / Error and a short reason.
	bool SaveSinglePackageWithRetry(UPackage* Package, FString& OutFilename, FString& OutReason)
	{
		UObject* Asset = PickPrimaryAsset(Package);
		if (!Asset)
		{
			OutReason = TEXT("no top-level asset in package");
			return false;
		}

		FString PackageExtension = Package->ContainsMap()
			? FPackageName::GetMapPackageExtension()
			: FPackageName::GetAssetPackageExtension();
		OutFilename = FPaths::ConvertRelativePathToFull(
			FPackageName::LongPackageNameToFilename(Package->GetName(), PackageExtension));

		// Clear stale read-only attribute (source control / LFS).
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		if (PF.FileExists(*OutFilename) && PF.IsReadOnly(*OutFilename))
		{
			PF.SetReadOnly(*OutFilename, false);
		}

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;

		// 3 attempts × 100ms gap rides out the typical Defender/Indexer hold (<200ms).
		ESavePackageResult Result = ESavePackageResult::Error;
		for (int32 Attempt = 0; Attempt < 3; ++Attempt)
		{
			if (Attempt > 0)
			{
				FPlatformProcess::Sleep(0.1f);
			}

#if PLATFORM_WINDOWS
			TrySavePackageEditorUtils(Package, Asset, *OutFilename, &SaveArgs, &Result);
#else
			Result = UPackage::Save(Package, Asset, *OutFilename, SaveArgs).Result;
#endif

			if (Result == ESavePackageResult::Success)
			{
				return true;
			}
		}

		OutReason = FString::Printf(TEXT("UPackage::Save failed after 3 attempts (result=%d) — file may be locked by Defender / Indexer / external editor"), (int32)Result);
		return false;
	}
}

FString FBlueprintMCPServer::HandleSaveAll(const FString& Body)
{
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: save_all()"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("save_all requires editor mode."));
	}

	// Iterate dirty packages directly instead of FEditorFileUtils::SaveDirtyPackages.
	// SaveDirtyPackages routes through the editor's UI layer, which surfaces a modal
	// "Failed to save" dialog on UPackage::Save errors (notably ERROR_SHARING_VIOLATION
	// when Windows Defender or the Search Indexer is holding the .uasset), and that
	// modal blocks the entire editor + the MCP server. Custom path: SAVE_NoError +
	// SEH-protected save + transient-failure retry, with a per-package failure list
	// returned in the JSON response so callers can act on partial failures.
	TArray<UPackage*> DirtyPackages;
	FEditorFileUtils::GetDirtyPackages(DirtyPackages);

	int32 SavedCount = 0;
	int32 SkippedCount = 0;
	TArray<TSharedPtr<FJsonValue>> Failures;

	for (UPackage* Package : DirtyPackages)
	{
		if (!Package) { ++SkippedCount; continue; }

		// Skip script / compiled-in / transient packages — they aren't meant to be saved.
		if (Package->HasAnyPackageFlags(PKG_CompiledIn) ||
			Package->HasAnyFlags(RF_Transient) ||
			Package == GetTransientPackage())
		{
			++SkippedCount;
			continue;
		}

		FString Filename;
		FString Reason;
		const bool bOk = SaveSinglePackageWithRetry(Package, Filename, Reason);
		if (bOk)
		{
			++SavedCount;
		}
		else
		{
			TSharedRef<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetStringField(TEXT("package"), Package->GetName());
			FailObj->SetStringField(TEXT("filename"), Filename);
			FailObj->SetStringField(TEXT("reason"), Reason);
			Failures.Add(MakeShared<FJsonValueObject>(FailObj));
			UE_LOG(LogTemp, Warning, TEXT("BlueprintMCP: save_all — '%s' failed: %s"), *Package->GetName(), *Reason);
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), Failures.Num() == 0);
	Result->SetNumberField(TEXT("savedCount"), SavedCount);
	Result->SetNumberField(TEXT("failedCount"), Failures.Num());
	Result->SetNumberField(TEXT("skippedCount"), SkippedCount);
	Result->SetArrayField(TEXT("failures"), Failures);

	return JsonToString(Result);
}

// ============================================================
// HandleGetDirtyPackages — list unsaved packages
// ============================================================

FString FBlueprintMCPServer::HandleGetDirtyPackages(const FString& Body)
{
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: get_dirty_packages()"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("get_dirty_packages requires editor mode."));
	}

	TArray<UPackage*> DirtyPackages;
	FEditorFileUtils::GetDirtyPackages(DirtyPackages);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), DirtyPackages.Num());

	TArray<TSharedPtr<FJsonValue>> PackageArray;
	for (UPackage* Package : DirtyPackages)
	{
		if (!Package) continue;

		TSharedRef<FJsonObject> PkgObj = MakeShared<FJsonObject>();
		PkgObj->SetStringField(TEXT("name"), Package->GetName());

		FString ResolvedFileName;
		if (FPackageName::DoesPackageExist(Package->GetName(), &ResolvedFileName))
		{
			PkgObj->SetStringField(TEXT("fileName"), ResolvedFileName);
		}

		PackageArray.Add(MakeShared<FJsonValueObject>(PkgObj));
	}
	Result->SetArrayField(TEXT("packages"), PackageArray);

	return JsonToString(Result);
}
