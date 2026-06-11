#include "UI/NiagaraPreviewPlayerWindow.h"

#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "DragAndDrop/ActorDragDropOp.h"
#include "DragAndDrop/AssetDragDropOp.h"
#include "DragAndDrop/CompositeDragDropOp.h"
#include "EditorViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "Input/DragAndDrop.h"
#include "Input/Reply.h"
#include "Misc/Paths.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "PreviewScene.h"
#include "SEditorViewport.h"
#include "Styling/CoreStyle.h"
#include "UObject/GCObject.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
    constexpr float PlayerWindowWidth = 900.0f;
    constexpr float PlayerWindowHeight = 620.0f;
    constexpr int32 PreviewFrameStabilizationTicks = 18;
    constexpr double PreviewBoundsCenterTolerance = 1.0;
    constexpr double PreviewBoundsExtentTolerance = 1.0;

    FString NormalizeNiagaraPreviewPlayerObjectPath(const FString& InputPath)
    {
        FString Path = InputPath.TrimStartAndEnd();
        Path.TrimQuotesInline();

        if ((Path.StartsWith(TEXT("/Game/")) || Path.StartsWith(TEXT("/Engine/")) || Path.StartsWith(TEXT("/Niagara/"))) && !Path.Contains(TEXT(".")))
        {
            const FString AssetName = FPaths::GetBaseFilename(Path);
            Path = FString::Printf(TEXT("%s.%s"), *Path, *AssetName);
        }

        return Path;
    }

    TSharedPtr<FAssetDragDropOp> GetAssetDragDropOp(const FDragDropEvent& DragDropEvent)
    {
        if (TSharedPtr<FAssetDragDropOp> AssetOp = DragDropEvent.GetOperationAs<FAssetDragDropOp>())
        {
            return AssetOp;
        }

        if (TSharedPtr<FCompositeDragDropOp> CompositeOp = DragDropEvent.GetOperationAs<FCompositeDragDropOp>())
        {
            return CompositeOp->GetSubOp<FAssetDragDropOp>();
        }

        return nullptr;
    }

    TSharedPtr<FActorDragDropOp> GetActorDragDropOp(const FDragDropEvent& DragDropEvent)
    {
        if (TSharedPtr<FActorDragDropOp> ActorOp = DragDropEvent.GetOperationAs<FActorDragDropOp>())
        {
            return ActorOp;
        }

        if (TSharedPtr<FCompositeDragDropOp> CompositeOp = DragDropEvent.GetOperationAs<FCompositeDragDropOp>())
        {
            return CompositeOp->GetSubOp<FActorDragDropOp>();
        }

        return nullptr;
    }

    bool CanAcceptDrop(const FDragDropEvent& DragDropEvent)
    {
        const TSharedPtr<FAssetDragDropOp> AssetOp = GetAssetDragDropOp(DragDropEvent);
        if (AssetOp.IsValid() && (AssetOp->HasAssets() || AssetOp->HasAssetPaths()))
        {
            return true;
        }

        const TSharedPtr<FActorDragDropOp> ActorOp = GetActorDragDropOp(DragDropEvent);
        return ActorOp.IsValid() && ActorOp->Actors.Num() > 0;
    }
}

class FNiagaraPreviewViewportClient : public FEditorViewportClient
{
public:
    FNiagaraPreviewViewportClient(FPreviewScene* InPreviewScene, const TWeakPtr<SEditorViewport>& InEditorViewportWidget)
        : FEditorViewportClient(nullptr, InPreviewScene, InEditorViewportWidget)
    {
        SetRealtime(true);
        SetViewMode(VMI_Lit);
        SetViewLocation(FVector(-360.0, -520.0, 240.0));
        SetViewRotation(FRotator(-18.0, 38.0, 0.0));
        SetLookAtLocation(FVector::ZeroVector);
        bSetListenerPosition = false;
        bUsingOrbitCamera = true;
        EngineShowFlags.EnableAdvancedFeatures();
        EngineShowFlags.SetLighting(true);
        EngineShowFlags.SetPostProcessing(true);
        EngineShowFlags.SetParticles(true);
        EngineShowFlags.SetGrid(false);
        EngineShowFlags.SetSelectionOutline(false);
    }

    virtual void Tick(float DeltaSeconds) override
    {
        FEditorViewportClient::Tick(DeltaSeconds);

        if (FPreviewScene* LocalPreviewScene = GetPreviewScene())
        {
            if (UWorld* World = LocalPreviewScene->GetWorld())
            {
                World->Tick(LEVELTICK_All, DeltaSeconds);
            }
        }
    }
};

class SNiagaraPreviewViewport : public SEditorViewport, public FGCObject
{
public:
    SLATE_BEGIN_ARGS(SNiagaraPreviewViewport)
    {
    }
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs)
    {
        (void)InArgs;
        PreviewScene = MakeUnique<FPreviewScene>(
            FPreviewScene::ConstructionValues()
            .SetCreateDefaultLighting(true)
            .SetLightBrightness(6.0f)
            .SetSkyBrightness(1.2f)
            .SetTransactional(false)
            .AllowAudioPlayback(false));

        SEditorViewport::Construct(SEditorViewport::FArguments());
    }

    virtual ~SNiagaraPreviewViewport() override
    {
        ClearPreviewSystem();
    }

    virtual void AddReferencedObjects(FReferenceCollector& Collector) override
    {
        Collector.AddReferencedObject(PreviewComponent);
        Collector.AddReferencedObject(PreviewSystem);
    }

    virtual FString GetReferencerName() const override
    {
        return TEXT("SNiagaraPreviewViewport");
    }

    bool SetPreviewSystem(UNiagaraSystem* NiagaraSystem)
    {
        ClearPreviewSystem();
        if (!NiagaraSystem || !PreviewScene.IsValid())
        {
            return false;
        }

        PreviewSystem = NiagaraSystem;
        PreviewComponent = NewObject<UNiagaraComponent>(GetTransientPackage(), NAME_None, RF_Transient);
        PreviewComponent->SetAsset(NiagaraSystem);
        PreviewComponent->SetForceSolo(true);
        PreviewComponent->SetAutoActivate(true);
        PreviewComponent->SetRelativeLocation(FVector::ZeroVector);
        PreviewComponent->SetRelativeRotation(FRotator::ZeroRotator);
        PreviewComponent->SetRelativeScale3D(FVector(1.0f));
        PreviewComponent->SetCanEverAffectNavigation(false);
        PreviewComponent->CastShadow = false;

        PreviewScene->AddComponent(PreviewComponent, FTransform::Identity);
        PreviewComponent->Activate(true);
        PreviewComponent->AdvanceSimulationByTime(0.25f, 1.0f / 30.0f);
        PreviewComponent->SetPaused(false);
        bPreviewPlaying = true;
        bLooping = true;
        FNiagaraPreviewPlayerWindow::SetLoopingState(bLooping);

        LastFramedBounds = FBoxSphereBounds(FSphere(FVector::ZeroVector, 0.0f));
        PendingFrameTicks = PreviewFrameStabilizationTicks;
        FramePreview(true);
        Invalidate();
        return true;
    }

    void TogglePlayPausePreview()
    {
        if (!PreviewComponent)
        {
            return;
        }

        if (bPreviewPlaying)
        {
            PreviewComponent->SetPaused(true);
            bPreviewPlaying = false;
            FNiagaraPreviewPlayerWindow::SetPlaybackState(TEXT("paused"));
        }
        else
        {
            if (PreviewComponent->IsComplete())
            {
                PreviewComponent->ResetSystem();
            }

            PreviewComponent->Activate(false);
            PreviewComponent->SetPaused(false);
            bPreviewPlaying = true;
            FNiagaraPreviewPlayerWindow::SetPlaybackState(TEXT("playing"));
        }

        Invalidate();
    }

    void SetLooping(bool bInLooping)
    {
        bLooping = bInLooping;
        FNiagaraPreviewPlayerWindow::SetLoopingState(bLooping);
        Invalidate();
    }

    void ClearPreviewSystem()
    {
        if (PreviewComponent && PreviewScene.IsValid())
        {
            PreviewComponent->DeactivateImmediate();
            PreviewScene->RemoveComponent(PreviewComponent);
        }

        PreviewComponent = nullptr;
        PreviewSystem = nullptr;
        bPreviewPlaying = false;
        bLooping = true;
        PendingFrameTicks = 0;
        LastFramedBounds = FBoxSphereBounds(FSphere(FVector::ZeroVector, 0.0f));
        Invalidate();
    }

    bool HasPreviewSystem() const
    {
        return PreviewComponent != nullptr && PreviewSystem != nullptr;
    }

    bool IsPreviewPlaying() const
    {
        return bPreviewPlaying;
    }

    bool IsLooping() const
    {
        return bLooping;
    }

    virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override
    {
        SEditorViewport::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

        const FVector2D ViewportSize = AllottedGeometry.GetLocalSize();
        if (ViewportSize.X > 1.0f && ViewportSize.Y > 1.0f)
        {
            const double NewAspectRatio = static_cast<double>(ViewportSize.X) / static_cast<double>(ViewportSize.Y);
            if (!FMath::IsNearlyEqual(NewAspectRatio, LastViewportAspectRatio, 0.01))
            {
                LastViewportAspectRatio = NewAspectRatio;
                if (PreviewComponent)
                {
                    FramePreview(true);
                }
            }
        }

        if (PreviewComponent && PendingFrameTicks > 0)
        {
            --PendingFrameTicks;
            FramePreview(false);
        }

        if (!PreviewComponent || !bPreviewPlaying || !PreviewComponent->IsComplete())
        {
            return;
        }

        if (bLooping)
        {
            PreviewComponent->ResetSystem();
            PreviewComponent->SetPaused(false);
            FNiagaraPreviewPlayerWindow::SetPlaybackState(TEXT("playing"));
        }
        else
        {
            PreviewComponent->SetPaused(true);
            bPreviewPlaying = false;
            FNiagaraPreviewPlayerWindow::SetPlaybackState(TEXT("stopped"));
        }

        Invalidate();
    }

protected:
    virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override
    {
        ViewportClient = MakeShared<FNiagaraPreviewViewportClient>(PreviewScene.Get(), SharedThis(this));
        return ViewportClient.ToSharedRef();
    }

private:
    void FramePreview(bool bForce)
    {
        if (!ViewportClient.IsValid() || !PreviewComponent)
        {
            return;
        }

        PreviewComponent->UpdateBounds();
        const FBoxSphereBounds Bounds = GetBestPreviewBounds();

        if (!bForce && !HasBoundsChangedEnough(Bounds))
        {
            return;
        }

        constexpr double FitPadding = 1.12;
        const FVector Center = Bounds.Origin;
        const FVector Extent = Bounds.BoxExtent.GetAbs() * FitPadding;
        const FVector ViewDirection = FVector(-0.56, -0.74, 0.36).GetSafeNormal();
        const FRotator CameraRotation = ViewDirection.Rotation();
        const FRotationMatrix CameraRotationMatrix(CameraRotation);
        const FVector CameraForward = CameraRotationMatrix.GetScaledAxis(EAxis::X).GetSafeNormal();
        const FVector CameraRight = CameraRotationMatrix.GetScaledAxis(EAxis::Y).GetSafeNormal();
        const FVector CameraUp = CameraRotationMatrix.GetScaledAxis(EAxis::Z).GetSafeNormal();

        const double AspectRatio = FMath::Max(LastViewportAspectRatio, 0.01);

        const double HorizontalHalfFovRadians = FMath::DegreesToRadians(static_cast<double>(ViewportClient->ViewFOV) * 0.5);
        const double HorizontalTan = FMath::Max(FMath::Tan(HorizontalHalfFovRadians), 0.01);
        const double VerticalTan = FMath::Max(HorizontalTan / FMath::Max(AspectRatio, 0.01), 0.01);
        const double MinDepthPadding = 8.0;

        double RequiredDistance = 0.0;
        for (int32 XSign = -1; XSign <= 1; XSign += 2)
        {
            for (int32 YSign = -1; YSign <= 1; YSign += 2)
            {
                for (int32 ZSign = -1; ZSign <= 1; ZSign += 2)
                {
                    const FVector CornerOffset(
                        Extent.X * static_cast<double>(XSign),
                        Extent.Y * static_cast<double>(YSign),
                        Extent.Z * static_cast<double>(ZSign));
                    const double ProjectedRight = FMath::Abs(FVector::DotProduct(CornerOffset, CameraRight));
                    const double ProjectedUp = FMath::Abs(FVector::DotProduct(CornerOffset, CameraUp));
                    const double ProjectedForward = FVector::DotProduct(CornerOffset, CameraForward);

                    RequiredDistance = FMath::Max(RequiredDistance, ProjectedRight / HorizontalTan - ProjectedForward);
                    RequiredDistance = FMath::Max(RequiredDistance, ProjectedUp / VerticalTan - ProjectedForward);
                    RequiredDistance = FMath::Max(RequiredDistance, -ProjectedForward + MinDepthPadding);
                }
            }
        }

        const double Distance = FMath::Clamp(RequiredDistance, 50.0, 9000.0);
        const FVector CameraLocation = Center - CameraForward * Distance;

        ViewportClient->SetViewLocation(CameraLocation);
        ViewportClient->SetViewRotation(CameraRotation);
        ViewportClient->SetLookAtLocation(Center);
        ViewportClient->Invalidate();
        LastFramedBounds = Bounds;
    }

    FBoxSphereBounds GetBestPreviewBounds() const
    {
        FBoxSphereBounds Bounds = PreviewComponent->CalcBounds(PreviewComponent->GetComponentTransform());
        if (Bounds.SphereRadius >= 8.0f && Bounds.SphereRadius <= 5000.0f)
        {
            return Bounds;
        }

        Bounds = PreviewComponent->Bounds;
        if (Bounds.SphereRadius >= 8.0f && Bounds.SphereRadius <= 5000.0f)
        {
            return Bounds;
        }

        return FBoxSphereBounds(FSphere(FVector::ZeroVector, 180.0f));
    }

    bool HasBoundsChangedEnough(const FBoxSphereBounds& Bounds) const
    {
        if (LastFramedBounds.SphereRadius <= KINDA_SMALL_NUMBER)
        {
            return true;
        }

        return FVector::DistSquared(Bounds.Origin, LastFramedBounds.Origin) > FMath::Square(PreviewBoundsCenterTolerance)
            || FVector::DistSquared(Bounds.BoxExtent, LastFramedBounds.BoxExtent) > FMath::Square(PreviewBoundsExtentTolerance)
            || FMath::Abs(Bounds.SphereRadius - LastFramedBounds.SphereRadius) > PreviewBoundsExtentTolerance;
    }

    TUniquePtr<FPreviewScene> PreviewScene;
    TSharedPtr<FNiagaraPreviewViewportClient> ViewportClient;
    TObjectPtr<UNiagaraComponent> PreviewComponent = nullptr;
    TObjectPtr<UNiagaraSystem> PreviewSystem = nullptr;
    bool bPreviewPlaying = false;
    bool bLooping = true;
    double LastViewportAspectRatio = 16.0 / 9.0;
    int32 PendingFrameTicks = 0;
    FBoxSphereBounds LastFramedBounds = FBoxSphereBounds(FSphere(FVector::ZeroVector, 0.0f));
};

class SNiagaraPreviewPlayerWidget : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SNiagaraPreviewPlayerWidget)
    {
    }
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs)
    {
        (void)InArgs;

        ChildSlot
        [
            SNew(SBorder)
            .Padding(12.0f)
            .BorderImage(FCoreStyle::Get().GetBrush(TEXT("ToolPanel.GroupBorder")))
            [
                SNew(SVerticalBox)

                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(TEXT("Niagara Preview Player")))
                    .Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 16))
                    .ColorAndOpacity(FLinearColor(0.90f, 0.95f, 1.0f, 1.0f))
                ]

                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 8.0f, 0.0f, 0.0f)
                [
                    SNew(SHorizontalBox)

                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .Padding(0.0f, 0.0f, 6.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text(this, &SNiagaraPreviewPlayerWidget::GetPlayPauseButtonText)
                        .IsEnabled(this, &SNiagaraPreviewPlayerWidget::ArePlaybackControlsEnabled)
                        .OnClicked(this, &SNiagaraPreviewPlayerWidget::OnPlayClicked)
                    ]

                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    [
                        SNew(SCheckBox)
                        .IsChecked(this, &SNiagaraPreviewPlayerWidget::GetLoopingCheckState)
                        .IsEnabled(this, &SNiagaraPreviewPlayerWidget::ArePlaybackControlsEnabled)
                        .OnCheckStateChanged(this, &SNiagaraPreviewPlayerWidget::OnLoopingChanged)
                        [
                            SNew(STextBlock)
                            .Text(FText::FromString(TEXT("Looping")))
                            .ColorAndOpacity(FLinearColor(0.78f, 0.84f, 0.88f, 1.0f))
                        ]
                    ]

                    + SHorizontalBox::Slot()
                    .FillWidth(1.0f)
                    .HAlign(HAlign_Right)
                    .VAlign(VAlign_Center)
                    [
                        SAssignNew(PlaybackStatusTextBlock, STextBlock)
                        .Text(this, &SNiagaraPreviewPlayerWidget::GetPlaybackStatusText)
                        .ColorAndOpacity(FLinearColor(0.64f, 0.72f, 0.78f, 1.0f))
                    ]
                ]

                + SVerticalBox::Slot()
                .FillHeight(1.0f)
                .Padding(0.0f, 10.0f, 0.0f, 10.0f)
                [
                    SNew(SOverlay)

                    + SOverlay::Slot()
                    [
                        SAssignNew(PreviewViewport, SNiagaraPreviewViewport)
                    ]

                    + SOverlay::Slot()
                    .HAlign(HAlign_Center)
                    .VAlign(VAlign_Center)
                    [
                        SNew(SBorder)
                        .Visibility(this, &SNiagaraPreviewPlayerWidget::GetViewportOverlayVisibility)
                        .Padding(18.0f)
                        .BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
                        .BorderBackgroundColor(FLinearColor(0.018f, 0.021f, 0.027f, 0.78f))
                        [
                            SNew(SVerticalBox)

                            + SVerticalBox::Slot()
                            .AutoHeight()
                            .HAlign(HAlign_Center)
                            [
                                SAssignNew(DropTitleTextBlock, STextBlock)
                                .Text(this, &SNiagaraPreviewPlayerWidget::GetDropSurfaceTitleText)
                                .Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 14))
                                .ColorAndOpacity(FLinearColor(0.82f, 0.88f, 0.92f, 1.0f))
                            ]

                            + SVerticalBox::Slot()
                            .AutoHeight()
                            .Padding(0.0f, 8.0f, 0.0f, 0.0f)
                            .HAlign(HAlign_Center)
                            [
                                SAssignNew(DropBodyTextBlock, STextBlock)
                                .Text(this, &SNiagaraPreviewPlayerWidget::GetDropSurfaceBodyText)
                                .ColorAndOpacity(FLinearColor(0.52f, 0.59f, 0.65f, 1.0f))
                            ]

                            + SVerticalBox::Slot()
                            .AutoHeight()
                            .Padding(0.0f, 16.0f, 0.0f, 0.0f)
                            .HAlign(HAlign_Center)
                            [
                                SAssignNew(DropDetailsTextBlock, STextBlock)
                                .Text(this, &SNiagaraPreviewPlayerWidget::GetDropDetailsText)
                                .Visibility(this, &SNiagaraPreviewPlayerWidget::GetDropDetailsVisibility)
                                .AutoWrapText(true)
                                .Justification(ETextJustify::Center)
                                .ColorAndOpacity(FLinearColor(0.74f, 0.86f, 0.92f, 1.0f))
                            ]
                        ]
                    ]
                ]

                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SAssignNew(StatusTextBlock, STextBlock)
                    .Text(this, &SNiagaraPreviewPlayerWidget::GetStatusText)
                    .AutoWrapText(true)
                    .ColorAndOpacity(FLinearColor(0.78f, 0.84f, 0.88f, 1.0f))
                ]
            ]
        ];
    }

    virtual FReply OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override
    {
        (void)MyGeometry;
        return CanAcceptDrop(DragDropEvent) ? FReply::Handled() : FReply::Unhandled();
    }

    virtual FReply OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override
    {
        (void)MyGeometry;

        if (const TSharedPtr<FAssetDragDropOp> AssetOp = GetAssetDragDropOp(DragDropEvent))
        {
            const TArray<FAssetData>& Assets = AssetOp->GetAssets();
            if (!Assets.IsEmpty())
            {
                FNiagaraPreviewPlayerWindow::RecordDroppedAsset(Assets[0]);
                PreviewDroppedAsset(Assets[0]);
                return FReply::Handled();
            }

            const TArray<FString>& AssetPaths = AssetOp->GetAssetPaths();
            if (!AssetPaths.IsEmpty())
            {
                FNiagaraPreviewPlayerWindow::RecordDroppedAssetPath(AssetPaths[0]);
                PreviewDroppedAssetPath(AssetPaths[0]);
                return FReply::Handled();
            }
        }

        if (const TSharedPtr<FActorDragDropOp> ActorOp = GetActorDragDropOp(DragDropEvent))
        {
            for (const TWeakObjectPtr<AActor>& WeakActor : ActorOp->Actors)
            {
                if (AActor* Actor = WeakActor.Get())
                {
                    FNiagaraPreviewPlayerWindow::RecordDroppedActor(Actor);
                    return FReply::Handled();
                }
            }
        }

        return FReply::Unhandled();
    }

private:
    static bool HasDrop()
    {
        return FNiagaraPreviewPlayerWindow::DropCount > 0;
    }

    FText GetDropSurfaceTitleText() const
    {
        if (!HasDrop())
        {
            return FText::FromString(TEXT("Drop a Niagara system, asset, or actor here"));
        }

        if (FNiagaraPreviewPlayerWindow::LastClassName.Contains(TEXT("NiagaraSystem")))
        {
            return FText::FromString(TEXT("Niagara system selected"));
        }

        if (FNiagaraPreviewPlayerWindow::LastDropKind == TEXT("actor"))
        {
            return FText::FromString(TEXT("Actor selected"));
        }

        return FText::FromString(TEXT("Asset selected"));
    }

    FText GetDropSurfaceBodyText() const
    {
        if (!HasDrop())
        {
            return FText::FromString(TEXT("Viewport rendering will attach here after this drop surface is verified."));
        }

        return FText::FromString(FString::Printf(
            TEXT("%s is ready for the next preview-player step."),
            *FNiagaraPreviewPlayerWindow::LastDisplayName));
    }

    EVisibility GetDropDetailsVisibility() const
    {
        return HasDrop() ? EVisibility::Visible : EVisibility::Collapsed;
    }

    EVisibility GetViewportOverlayVisibility() const
    {
        return bHasRenderablePreview ? EVisibility::Collapsed : EVisibility::Visible;
    }

    FText GetDropDetailsText() const
    {
        if (!HasDrop())
        {
            return FText::GetEmpty();
        }

        return FText::FromString(FString::Printf(
            TEXT("%s\n%s"),
            *FNiagaraPreviewPlayerWindow::LastClassName,
            *FNiagaraPreviewPlayerWindow::LastObjectPath));
    }

    FText GetStatusText() const
    {
        if (FNiagaraPreviewPlayerWindow::DropCount <= 0)
        {
            return FText::FromString(TEXT("Ready. Waiting for the first drop."));
        }

        return FText::FromString(FString::Printf(
            TEXT("Last drop: %s | %s | %s"),
            *FNiagaraPreviewPlayerWindow::LastDropKind,
            *FNiagaraPreviewPlayerWindow::LastDisplayName,
            *FNiagaraPreviewPlayerWindow::LastClassName));
    }

    FText GetPlaybackStatusText() const
    {
        if (!PreviewViewport.IsValid() || !PreviewViewport->HasPreviewSystem())
        {
            return FText::FromString(TEXT("No preview loaded"));
        }

        return FText::FromString(FString::Printf(
            TEXT("%s | Looping %s"),
            PreviewViewport->IsPreviewPlaying() ? TEXT("Playing") : TEXT("Paused"),
            PreviewViewport->IsLooping() ? TEXT("On") : TEXT("Off")));
    }

    FText GetPlayPauseButtonText() const
    {
        return PreviewViewport.IsValid() && PreviewViewport->IsPreviewPlaying()
            ? FText::FromString(TEXT("Pause"))
            : FText::FromString(TEXT("Play"));
    }

    bool ArePlaybackControlsEnabled() const
    {
        return PreviewViewport.IsValid() && PreviewViewport->HasPreviewSystem();
    }

    ECheckBoxState GetLoopingCheckState() const
    {
        return PreviewViewport.IsValid() && PreviewViewport->IsLooping()
            ? ECheckBoxState::Checked
            : ECheckBoxState::Unchecked;
    }

    FReply OnPlayClicked()
    {
        if (PreviewViewport.IsValid())
        {
            PreviewViewport->TogglePlayPausePreview();
            RefreshDropState();
        }

        return FReply::Handled();
    }

    void OnLoopingChanged(ECheckBoxState NewState)
    {
        if (PreviewViewport.IsValid())
        {
            PreviewViewport->SetLooping(NewState == ECheckBoxState::Checked);
            RefreshDropState();
        }
    }

public:
    void RefreshDropState()
    {
        if (DropTitleTextBlock.IsValid())
        {
            DropTitleTextBlock->Invalidate(EInvalidateWidgetReason::Paint);
        }
        if (DropBodyTextBlock.IsValid())
        {
            DropBodyTextBlock->Invalidate(EInvalidateWidgetReason::Paint);
        }
        if (DropDetailsTextBlock.IsValid())
        {
            DropDetailsTextBlock->Invalidate(EInvalidateWidgetReason::PaintAndVolatility);
        }
        if (StatusTextBlock.IsValid())
        {
            StatusTextBlock->Invalidate(EInvalidateWidgetReason::Paint);
        }
        if (PlaybackStatusTextBlock.IsValid())
        {
            PlaybackStatusTextBlock->Invalidate(EInvalidateWidgetReason::Paint);
        }
        Invalidate(EInvalidateWidgetReason::LayoutAndVolatility);
    }

    bool PreviewSystem(UNiagaraSystem* NiagaraSystem)
    {
        bHasRenderablePreview = false;
        if (!PreviewViewport.IsValid())
        {
            FNiagaraPreviewPlayerWindow::bLastPreviewRenderable = false;
            return false;
        }

        bHasRenderablePreview = PreviewViewport->SetPreviewSystem(NiagaraSystem);
        FNiagaraPreviewPlayerWindow::bLastPreviewRenderable = bHasRenderablePreview;
        FNiagaraPreviewPlayerWindow::SetPlaybackState(bHasRenderablePreview ? TEXT("playing") : TEXT("none"));
        RefreshDropState();
        return bHasRenderablePreview;
    }

    void PreviewDroppedAsset(const FAssetData& AssetData)
    {
        UObject* LoadedAsset = AssetData.GetAsset();
        UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(LoadedAsset);
        if (!NiagaraSystem)
        {
            if (PreviewViewport.IsValid())
            {
                PreviewViewport->ClearPreviewSystem();
            }
            FNiagaraPreviewPlayerWindow::bLastPreviewRenderable = false;
            FNiagaraPreviewPlayerWindow::SetPlaybackState(TEXT("none"));
            return;
        }

        PreviewSystem(NiagaraSystem);
    }

    void PreviewDroppedAssetPath(const FString& AssetPath)
    {
        const FString ObjectPath = NormalizeNiagaraPreviewPlayerObjectPath(AssetPath);
        UNiagaraSystem* NiagaraSystem = LoadObject<UNiagaraSystem>(nullptr, *ObjectPath);
        if (!NiagaraSystem)
        {
            if (PreviewViewport.IsValid())
            {
                PreviewViewport->ClearPreviewSystem();
            }
            FNiagaraPreviewPlayerWindow::bLastPreviewRenderable = false;
            FNiagaraPreviewPlayerWindow::SetPlaybackState(TEXT("none"));
            RefreshDropState();
            return;
        }

        FNiagaraPreviewPlayerWindow::LastDisplayName = NiagaraSystem->GetName();
        FNiagaraPreviewPlayerWindow::LastObjectPath = NiagaraSystem->GetPathName();
        FNiagaraPreviewPlayerWindow::LastClassName = NiagaraSystem->GetClass() ? NiagaraSystem->GetClass()->GetPathName() : FString();
        PreviewSystem(NiagaraSystem);
    }

private:
    TSharedPtr<SNiagaraPreviewViewport> PreviewViewport;
    TSharedPtr<STextBlock> DropTitleTextBlock;
    TSharedPtr<STextBlock> DropBodyTextBlock;
    TSharedPtr<STextBlock> DropDetailsTextBlock;
    TSharedPtr<STextBlock> StatusTextBlock;
    TSharedPtr<STextBlock> PlaybackStatusTextBlock;
    bool bHasRenderablePreview = false;
};

void FNiagaraPreviewPlayerWindow::Show()
{
    if (!FSlateApplication::IsInitialized())
    {
        return;
    }

    if (PlayerWindow.IsValid())
    {
        PresentPlayerWindow(PlayerWindow.ToSharedRef());
        return;
    }

    TSharedPtr<SNiagaraPreviewPlayerWidget> Widget;
    TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(FText::FromString(TEXT("Niagara Preview Player")))
        .AutoCenter(EAutoCenter::PreferredWorkArea)
        .ClientSize(FVector2D(PlayerWindowWidth, PlayerWindowHeight))
        .SizingRule(ESizingRule::UserSized)
        .SupportsMaximize(true)
        .SupportsMinimize(true)
        .HasCloseButton(true)
        .FocusWhenFirstShown(true)
        [
            SAssignNew(Widget, SNiagaraPreviewPlayerWidget)
        ];

    Window->SetOnWindowClosed(FOnWindowClosed::CreateStatic(&FNiagaraPreviewPlayerWindow::OnPlayerWindowClosed));
    PlayerWindow = Window;
    PlayerWidget = Widget;

    AddPlayerWindow(Window);
    PresentPlayerWindow(Window);
}

bool FNiagaraPreviewPlayerWindow::PreviewSystemByPath(const FString& SystemPath, FString& OutError)
{
    Show();

    const FString ObjectPath = NormalizeNiagaraPreviewPlayerObjectPath(SystemPath);
    UNiagaraSystem* NiagaraSystem = LoadObject<UNiagaraSystem>(nullptr, *ObjectPath);
    if (!NiagaraSystem)
    {
        OutError = FString::Printf(TEXT("Failed to load Niagara system: %s"), *ObjectPath);
        return false;
    }

    RecordDroppedAsset(FAssetData(NiagaraSystem));

    const TSharedPtr<SNiagaraPreviewPlayerWidget> Widget = PlayerWidget.Pin();
    if (!Widget.IsValid())
    {
        OutError = TEXT("Niagara Preview Player widget is not available");
        bLastPreviewRenderable = false;
        return false;
    }

    if (!Widget->PreviewSystem(NiagaraSystem))
    {
        OutError = FString::Printf(TEXT("Loaded Niagara system but could not attach it to the Preview Player viewport: %s"), *ObjectPath);
        return false;
    }

    OutError.Reset();
    return true;
}

TSharedPtr<FJsonObject> FNiagaraPreviewPlayerWindow::GetStateJson()
{
    TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
    State->SetBoolField(TEXT("success"), true);
    State->SetBoolField(TEXT("window_open"), PlayerWindow.IsValid());
    State->SetStringField(TEXT("player_mode"), TEXT("preview_scene_viewport"));
    State->SetBoolField(TEXT("last_preview_renderable"), bLastPreviewRenderable);
    State->SetNumberField(TEXT("drop_count"), DropCount);
    State->SetStringField(TEXT("last_drop_kind"), LastDropKind);
    State->SetStringField(TEXT("last_display_name"), LastDisplayName);
    State->SetStringField(TEXT("last_object_path"), LastObjectPath);
    State->SetStringField(TEXT("last_class_name"), LastClassName);
    State->SetStringField(TEXT("playback_state"), LastPlaybackState);
    State->SetBoolField(TEXT("looping"), bLastLooping);
    return State;
}

void FNiagaraPreviewPlayerWindow::AddPlayerWindow(const TSharedRef<SWindow>& Window)
{
    FSlateApplication& SlateApplication = FSlateApplication::Get();
    if (const TSharedPtr<SWindow> ParentWindow = GetTargetParentWindow())
    {
        SlateApplication.AddWindowAsNativeChild(Window, ParentWindow.ToSharedRef(), false);
        return;
    }

    SlateApplication.AddWindow(Window, false);
}

void FNiagaraPreviewPlayerWindow::PresentPlayerWindow(const TSharedRef<SWindow>& Window)
{
    Window->ShowWindow();
    Window->BringToFront();

    FSlateApplication& SlateApplication = FSlateApplication::Get();
    SlateApplication.PumpMessages();
    SlateApplication.ForceRedrawWindow(Window);
}

TSharedPtr<SWindow> FNiagaraPreviewPlayerWindow::GetTargetParentWindow()
{
    TSharedPtr<SWindow> ParentWindow = FSlateApplication::Get().FindBestParentWindowForDialogs(nullptr);
    if (ParentWindow == PlayerWindow)
    {
        ParentWindow.Reset();
    }

    return ParentWindow;
}

void FNiagaraPreviewPlayerWindow::OnPlayerWindowClosed(const TSharedRef<SWindow>& ClosedWindow)
{
    if (PlayerWindow.IsValid() && PlayerWindow.Get() == &ClosedWindow.Get())
    {
        PlayerWindow.Reset();
        PlayerWidget.Reset();
    }
}

void FNiagaraPreviewPlayerWindow::RecordDroppedActor(AActor* Actor)
{
    if (!Actor)
    {
        return;
    }

    LastDropKind = TEXT("actor");
    LastDisplayName = Actor->GetActorLabel();
    LastObjectPath = Actor->GetPathName();
    LastClassName = Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString();
    bLastPreviewRenderable = false;
    SetPlaybackState(TEXT("none"));
    ++DropCount;
    NotifyDropStateChanged();
}

void FNiagaraPreviewPlayerWindow::RecordDroppedAsset(const FAssetData& AssetData)
{
    LastDropKind = TEXT("asset");
    LastDisplayName = AssetData.AssetName.ToString();
    LastObjectPath = AssetData.GetObjectPathString();
    LastClassName = AssetData.AssetClassPath.ToString();
    bLastPreviewRenderable = false;
    SetPlaybackState(TEXT("none"));
    ++DropCount;
    NotifyDropStateChanged();
}

void FNiagaraPreviewPlayerWindow::RecordDroppedAssetPath(const FString& AssetPath)
{
    LastDropKind = TEXT("asset_path");
    LastDisplayName = FPaths::GetBaseFilename(AssetPath);
    LastObjectPath = AssetPath;
    LastClassName = TEXT("");
    bLastPreviewRenderable = false;
    SetPlaybackState(TEXT("none"));
    ++DropCount;
    NotifyDropStateChanged();
}

void FNiagaraPreviewPlayerWindow::SetPlaybackState(const FString& PlaybackState)
{
    LastPlaybackState = PlaybackState;
}

void FNiagaraPreviewPlayerWindow::SetLoopingState(bool bLooping)
{
    bLastLooping = bLooping;
}

void FNiagaraPreviewPlayerWindow::NotifyDropStateChanged()
{
    if (const TSharedPtr<SNiagaraPreviewPlayerWidget> Widget = PlayerWidget.Pin())
    {
        Widget->RefreshDropState();
    }

    if (PlayerWindow.IsValid() && FSlateApplication::IsInitialized())
    {
        FSlateApplication& SlateApplication = FSlateApplication::Get();
        SlateApplication.ForceRedrawWindow(PlayerWindow.ToSharedRef());
    }
}

void FNiagaraPreviewPlayerWindow::ResetDropState()
{
    LastDropKind.Reset();
    LastDisplayName.Reset();
    LastObjectPath.Reset();
    LastClassName.Reset();
    LastPlaybackState = TEXT("none");
    bLastLooping = true;
    bLastPreviewRenderable = false;
    DropCount = 0;
}

TSharedPtr<SWindow> FNiagaraPreviewPlayerWindow::PlayerWindow;
TWeakPtr<SNiagaraPreviewPlayerWidget> FNiagaraPreviewPlayerWindow::PlayerWidget;
FString FNiagaraPreviewPlayerWindow::LastDropKind;
FString FNiagaraPreviewPlayerWindow::LastDisplayName;
FString FNiagaraPreviewPlayerWindow::LastObjectPath;
FString FNiagaraPreviewPlayerWindow::LastClassName;
FString FNiagaraPreviewPlayerWindow::LastPlaybackState = TEXT("none");
bool FNiagaraPreviewPlayerWindow::bLastLooping = true;
bool FNiagaraPreviewPlayerWindow::bLastPreviewRenderable = false;
int32 FNiagaraPreviewPlayerWindow::DropCount = 0;
