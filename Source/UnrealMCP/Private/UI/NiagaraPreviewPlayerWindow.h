#pragma once

#include "CoreMinimal.h"

class FJsonObject;

class FNiagaraPreviewPlayerWindow
{
public:
    static void Show();
    static void Shutdown();
    static bool PreviewSystemByPath(const FString& SystemPath, FString& OutError);
    static TSharedPtr<FJsonObject> GetStateJson();

private:
    static void AddPlayerWindow(const TSharedRef<class SWindow>& Window);
    static void PresentPlayerWindow(const TSharedRef<class SWindow>& Window);
    static TSharedPtr<class SWindow> GetTargetParentWindow();
    static void OnPlayerWindowClosed(const TSharedRef<class SWindow>& ClosedWindow);
    static void RecordDroppedActor(class AActor* Actor);
    static void RecordDroppedAsset(const struct FAssetData& AssetData);
    static void RecordDroppedAssetPath(const FString& AssetPath);
    static void SetPlaybackState(const FString& PlaybackState);
    static void SetLoopingState(bool bLooping);
    static void UpdateAnalysisForSystem(class UNiagaraSystem* NiagaraSystem);
    static void NotifyDropStateChanged();
    static void ResetDropState();

    friend class SNiagaraPreviewPlayerWidget;
    friend class SNiagaraPreviewViewport;

    static TSharedPtr<class SWindow> PlayerWindow;
    static TWeakPtr<class SNiagaraPreviewPlayerWidget> PlayerWidget;
    static FString LastDropKind;
    static FString LastDisplayName;
    static FString LastObjectPath;
    static FString LastClassName;
    static FString LastPlaybackState;
    static FString LastAnalysisSummary;
    static bool bLastLooping;
    static bool bLastPreviewRenderable;
    static int32 DropCount;
};
