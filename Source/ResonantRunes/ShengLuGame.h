#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/GameModeBase.h"
#include "AbilitySystemInterface.h"
#include "Abilities/GameplayAbility.h"
#include "ShengLuRules.h"
#include "ShengLuGame.generated.h"

class ARuneBoxPawn;
class UProceduralMeshComponent;
class UCameraComponent;
class UAbilitySystemComponent;
class UAnimSequence;
class UMaterialInterface;
class UNiagaraComponent;
class UNiagaraSystem;
class UCapsuleComponent;
class UDecalComponent;
class UAudioComponent;
class USoundBase;
class UPointLightComponent;
class UCurveTable;
class UInstancedStaticMeshComponent;
class UPostProcessComponent;
class UTextureRenderTarget2D;

enum class ECastPhase : uint8 { Ready, Start, Loop, End };

UCLASS()
class RESONANTRUNES_API AShengLuBoard : public APawn, public IAbilitySystemInterface
{
    GENERATED_BODY()
public:
    AShengLuBoard();
    void BeginPlay() override;
    void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    void Tick(float DeltaSeconds) override;
    void CalcCamera(float DeltaSeconds, FMinimalViewInfo& OutView) override;
    void SetupPlayerInputComponent(UInputComponent* Input) override;
    UAbilitySystemComponent* GetAbilitySystemComponent() const override;
    FVector Cell(FIntVector Coord) const;
    FVector BoxCenter(FIntVector Coord) const;
    void SelectSkill(ERuneAction Action);
    void Confirm();
    bool Apply(ERuneAction Action);
    UFUNCTION(BlueprintCallable, Category="Puzzle") bool TryPuzzleAction(FPuzzleAction Action);
    void Undo();
    void ResetPuzzle();
    void Cancel();
    void ClickBoard();
    void ClickAt(FVector2D Mouse);
    void ReleaseClick();
    FPuzzleActionResult Preview() const;
    FPuzzleActionResult Evaluate(FPuzzleAction Action) const;
    bool IsFirstPerson() const;
    bool IsPawnSelected() const {return bPawnSelected;}
    UFUNCTION(BlueprintPure, Category="Puzzle") bool IsPlayerLevitating() const;
    bool IsDepthViewActive() const { return bDepthViewActive; }
    TOptional<FIntVector> StepDestination(ERuneAction Action) const;
    bool CanStepTo(FIntPoint P) const;
    TOptional<FIntVector> HoverCell() const;
    TOptional<FIntVector> InspectCellAt(FVector2D ScreenPosition) const;
    static constexpr float PulseHitSeconds = 1.f;
    float BeamProgress() const { return HitTime / PulseHitSeconds; }
    ECastPhase Phase() const {return CastPhase;}
    bool IsFiring() const {return bHeld && bLoopingBeam;}
    const UNiagaraComponent* ActiveContact() const {return LiveContact;}
    bool AimedAtBox() const {return bAimBox;}
    bool AimedAtSelf() const { return Selected == ERuneAction::Levitate && TargetBoxId == INDEX_NONE && TargetCell == State.PlayerPosition; }
    bool AimedAtObstacle() const { return !bAimBox && CanAimAtObstacle(); }
    int32 AimBoxId() const { return TargetBoxId; }
    const FIntVector& AimCell() const {return TargetCell;}
    bool LaunchWater();
    const FRuneBoard& Board() const { return LegacyView; }
    const FPuzzleRuntimeState& Runtime() const { return State; }
    const FPuzzleLevelDefinition& Level() const { return Definition; }
    bool UsesLegacyPresentation() const { return !LevelAsset; }
    const UPuzzleLevelAsset* NextPuzzleLevel() const { return NextLevelAsset; }
    bool CanAdvanceLevel() const;
    bool CanFinishCampaign() const;
    bool ShouldShowCampaignVictory() const;
    bool IsShowingResult() const { return CanAdvanceLevel() || ShouldShowCampaignVictory(); }
    void FinishCampaign();
    void DismissCampaignVictory();
    int32 ConfirmedSkillSlot() const { return LastConfirmedSkillSlot; }
    double SkillConfirmedTime() const { return LastSkillConfirmedTime; }
    UFUNCTION(BlueprintCallable, Category="Puzzle") void AdvanceLevel();
    UFUNCTION(BlueprintPure, Category="Puzzle") bool IsRestored() const { return FPuzzleSolver::IsRestored(Definition, State); }
    UFUNCTION(BlueprintPure, Category="Puzzle") FPuzzleRuntimeState GetPuzzleRuntimeState() const { return State; }
    UFUNCTION(BlueprintPure, Category="Puzzle") const UPuzzleLevelAsset* GetLevelAsset() const { return LevelAsset; }
    const TOptional<ERuneAction>& Selection() const { return Selected; }
    const FString& Message() const { return Notice; }
    bool IsAnimating() const { return MotionRemaining > 0.f; }
    bool HasHistory() const { return !History.IsEmpty(); }
    const UMeshComponent* CrateMesh() const;
    UStaticMeshComponent* BoxMesh(int32 BoxId) const;
    FIntVector PulseDirection() const;

private:
    friend class AShengLuGameMode;
    struct FActionSnapshot;
    void MoveForward(); void MoveBackward(); void MoveLeft(); void MoveRight();
    void MoveFromInput(ERuneAction Direction);
    void SelectPlayerCell();
    bool IsLevelCleared() const;
    void AcceptResultFromInput();
    void RetryFromInput();
    void UndoFromInput();
    void CancelFromInput();
    void UpdatePointerMode(bool bForce = false);
    void RecordConfirmedSkill(ERuneAction Skill);
    void SelectWind(); void SelectPulse(); void SelectWater();
    void SelectHiss();
    void DispelPlayerWind();
    void CycleAimBox();
    void BuildAssetPresentation();
    bool SpawnBoxPawns();
    void UpdateBoardPresentation(bool bSnapToGrid);
    void SetupBoardMarkers();
    FVector GoalMarkerCenter(FIntVector Position) const;
    void RebuildGoalMarkers(bool bSolved);
    void UpdateBoardMarkers();
    void UpdateObstaclePresentation();
    void UpdateHissEffect(bool bActive, bool bReset);
    void UpdateBoardCamera(float DeltaSeconds);
    void ZoomOverview(float Wheel);
    FPuzzleAction MakeAction(ERuneAction Action) const;
    FVector WorldFacing() const;
    FVector ToGridDirection(FVector Direction) const;
    FIntVector SurfaceUnder(FIntVector Position) const;
    void SetupWater();
    void StartWater();
    void UpdateWater(float Dt);
    void StopWaterHands();
    void UpdateWaterHandInputs();
    void ClearWater();
    bool WaterInFlight() const;
    void RestoreVisuals(const FActionSnapshot* Snapshot=nullptr);
    void DrawEffects(float DeltaSeconds);
    void SetupCasting();
    void SetupCastAudio();
    void PlaySound(UAudioComponent* Voice,FName Sound);
    void BeginWind(FVector Foot, int32 BoxId, float Age=0.f);
    void ClearWind();
    void UpdateWindCamera(UNiagaraComponent* Effect) const;
    void UpdateWind(float Dt);
    void SyncPlayerWindState();
    void UpdatePlayerWind();
    UNiagaraComponent* CastSystem(FName Name,const TCHAR* Family,const TCHAR* Effect);
    void UpdateCasting(float Dt);
    void Turn(ERuneAction Direction);
    void MouseX(float Delta);
    void MouseY(float Delta);
    void UpdateAim();
    void SetView(bool FirstPerson);
    void ExitFirstPerson();
    void StartCast(bool bKeepHandTransition = false);
    void EndCast();
    void ResetCast(TOptional<ERuneAction> EquippedSkill);
    void PlayHandPhase(ECastPhase NewPhase);
    UAnimSequence* GetCastClip(ECastPhase Phase) const;
    void ApplyHandPose();
    void PlayHandTransition(UAnimSequence* Clip);
    FVector HandAnchor() const;
    static constexpr float ChargeSeconds=.5f;
    int32 WaveSeed=0;
    void DrawPulse();
    void FadeContact();
    void UpdateScars();
    void SetRangeVisible(bool Visible);
    void SetBeamVisual(bool Visible);
    void UpdateLook(bool FrameTarget);
    TOptional<FIntVector> PickTile(const FHitResult& Hit) const;
    TOptional<FIntVector> PickObstacle(const FHitResult& Hit) const;
    bool CanAimAtObstacle() const;
    UStaticMeshComponent* Piece(FName Name, FVector Location, FVector Extent, FLinearColor Tint);
    UPROPERTY() TObjectPtr<UCameraComponent> Camera;
    UPROPERTY() TObjectPtr<UCameraComponent> Eyes;
    UPROPERTY() TObjectPtr<USkeletalMeshComponent> Hands;
    UPROPERTY() TObjectPtr<UCapsuleComponent> HeroPick;
    UPROPERTY() TArray<TObjectPtr<UAnimSequence>> CastClips;
    UPROPERTY() TArray<TObjectPtr<UAnimSequence>> HissClips;
    UPROPERTY() TObjectPtr<UAnimSequence> EquipHands;
    UPROPERTY() TObjectPtr<UAnimSequence> JumpStartHands;
    UPROPERTY() TObjectPtr<UAnimSequence> JumpLoopHands;
    UPROPERTY() TObjectPtr<UAnimSequence> JumpEndHands;
    UPROPERTY() TObjectPtr<UAudioComponent> CastTransient;
    UPROPERTY() TObjectPtr<UAudioComponent> CastLoop;
    UPROPERTY() TObjectPtr<UAudioComponent> ImpactLoop;
    UPROPERTY() TObjectPtr<UAudioComponent> WindLoop;
    UPROPERTY() TMap<FName,TObjectPtr<USoundBase>> CastSounds;
    UPROPERTY() TObjectPtr<UPointLightComponent> HandGlow;
    UPROPERTY() TObjectPtr<UPointLightComponent> ContactGlow;
    UPROPERTY() TObjectPtr<UCurveTable> HitCurves;
    UPROPERTY() TObjectPtr<UCurveTable> ChargeCurves;
    UPROPERTY() TObjectPtr<UCurveTable> TrailCurves;
    UPROPERTY() TObjectPtr<UCurveTable> ArmCurves;
    UPROPERTY() TMap<FName,TSoftObjectPtr<UNiagaraSystem>> EffectAssets;
    UPROPERTY() TObjectPtr<UNiagaraComponent> Range;
    UPROPERTY() TObjectPtr<UNiagaraComponent> PlayerWind;
    UPROPERTY() TObjectPtr<UNiagaraComponent> Charge;
    UPROPERTY() TObjectPtr<UNiagaraComponent> ChargeArm;
    UPROPERTY() TObjectPtr<UNiagaraComponent> BeamArm;
    UPROPERTY() TObjectPtr<UNiagaraComponent> BeamEnd;
    UPROPERTY() TObjectPtr<UAbilitySystemComponent> Abilities;
    UPROPERTY() TObjectPtr<USkeletalMeshComponent> Hero;
    UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category="Puzzle", meta=(AllowPrivateAccess="true")) TObjectPtr<UPuzzleLevelAsset> LevelAsset;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Tiles;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Crate;
    UPROPERTY() TMap<int32, TObjectPtr<ARuneBoxPawn>> BoxPawns;
    UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> Floors;
    UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> Walls;
    UPROPERTY() TMap<FIntVector, TObjectPtr<UStaticMeshComponent>> HissWallMeshes;
    UPROPERTY() TObjectPtr<UMaterialInterface> HissSurface;
    UPROPERTY() TObjectPtr<UNiagaraComponent> HissFlow;
    UPROPERTY() TObjectPtr<UPostProcessComponent> HissPost;
    UPROPERTY() TObjectPtr<UTextureRenderTarget2D> HissComposite;
    UPROPERTY() TObjectPtr<UTextureRenderTarget2D> HissDepth;
    TArray<FIntVector> FloorPositions, WallPositions;
    UPROPERTY() TObjectPtr<UStaticMesh> Cube;
    UPROPERTY() TObjectPtr<UMaterialInterface> Stone;
    UPROPERTY() TObjectPtr<UMaterialInterface> WindMat;
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> Ribbons;
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> BoardMarkers;
    UPROPERTY() TObjectPtr<UMaterialInterface> BoardMarkerMaterial;
    UPROPERTY() TObjectPtr<UMaterialInterface> BoardMarkerXRayMaterial;
    TArray<FIntVector, TInlineAllocator<4>> MarkedSteps;
    TOptional<FIntVector> MarkedHover;
    TOptional<FIntVector> MarkedArrival;
    bool bDepthViewActive = false;
    UPROPERTY() TObjectPtr<UNiagaraComponent> Tornado;
    UPROPERTY() TObjectPtr<UNiagaraComponent> Lightning;
    UPROPERTY() TObjectPtr<UNiagaraComponent> PulseImpact;
    UPROPERTY() TObjectPtr<UNiagaraComponent> LiveContact;
    UPROPERTY() TObjectPtr<UNiagaraComponent> WaterCharge;
    UPROPERTY() TObjectPtr<UNiagaraComponent> WaterChargeArm;
    UPROPERTY() TObjectPtr<UNiagaraComponent> WaterHold;
    UPROPERTY() TObjectPtr<UNiagaraComponent> WaterHoldArm;
    UPROPERTY() TObjectPtr<UPointLightComponent> WaterPalmFlash;
    UPROPERTY() TObjectPtr<UNiagaraComponent> WaterRelease;
    UPROPERTY() TObjectPtr<UNiagaraComponent> WaterBullet;
    UPROPERTY() TObjectPtr<UNiagaraComponent> WaterBurst;
    UPROPERTY() TObjectPtr<UNiagaraComponent> FallingAnchor;
    UPROPERTY() TObjectPtr<UNiagaraComponent> AnchorHit;
    UPROPERTY() TObjectPtr<UAudioComponent> WaterFlightAudio;
    UPROPERTY() TObjectPtr<UCurveTable> WaterAnchorCurves;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> WaterRange;
    FVector WaterFrom=FVector::ZeroVector, WaterTarget=FVector::ZeroVector;
    FVector WaterAnchorTarget=FVector::ZeroVector;
    FVector WaterNormal=FVector::UpVector;
    float WaterAge=-1.f, WaterTravel=0.f;
    bool bWaterHitBox=false, bWaterBurst=false, bAnchorLanded=false;
    int32 WaterBoxId = INDEX_NONE;
    struct FFadingContact
    {
        TWeakObjectPtr<UNiagaraComponent> FX;
        float Deadline;
    };
    TArray<FFadingContact> FadingContacts;
    struct FContactPuff
    {
        FVector Origin, Radial, Normal;
        float Birth, Lifetime, Radius, Roll;
    };
    TArray<FContactPuff> ContactPuffs;
    struct FContactArc
    {
        TWeakObjectPtr<UPrimitiveComponent> Surface;
        FVector Origin, Normal, Radial;
        float Birth, Life, Reach;
        int32 Seed;
    };
    TArray<FContactArc> ContactArcs;
    TWeakObjectPtr<UPrimitiveComponent> ContactSurface;
    FVector ContactNormal=FVector::UpVector;
    float BranchRemainder=0;
    int32 BranchSerial=0;
    int32 PuffSerial=0;
    float ContactStart=0, NextPuff=0, BeamFadeUntil=-1;
    bool bReleasedCharged=false;
    UPROPERTY() TObjectPtr<UAnimSequence> Idle;
    UPROPERTY() TObjectPtr<UAnimSequence> Walk;
    FPuzzleLevelDefinition Definition;
    FPuzzleRuntimeState State;
    FRuneBoard LegacyView;
    bool bPuzzleReady = false;
    bool bTravelRequested = false;
    bool bFinalCampaignLevel = false;
    bool bCampaignVictoryDismissed = false;
    int32 LastConfirmedSkillSlot = 0;
    double LastSkillConfirmedTime = -10.;
    UPROPERTY(Transient) TObjectPtr<UPuzzleLevelAsset> NextLevelAsset;
    struct FActionSnapshot
    {
        FPuzzleRuntimeState Board;
        FVector WindFoot;
        float WindAge, WindFadeAge;
        int32 WindBoxId;
    };
    TArray<FActionSnapshot> History;
    TOptional<ERuneAction> Selected;
    FString Notice;
    struct FBoxMotion { int32 BoxId; FVector From, Via, To; };
    TArray<FBoxMotion> BoxMotions;
    FVector HeroFrom, HeroTo;
    float MotionRemaining = 0.f;
    float OverviewZoom = 1.f;
    float TargetOverviewZoom = 1.f;
    float OverviewZoomLimit = 1.70f;
    float EffectTime = 0.f;
    float WindAge = -1.f;
    float WindFadeAge = -1.f;
    float AimAge = 0.f;
    FVector WindFoot = FVector::ZeroVector;
    int32 WindBoxId = INDEX_NONE;
    int32 TargetBoxId = INDEX_NONE;
    TOptional<int32> PreferredBoxId;
    bool bWalking = false;
    bool bHandTransitionPlaying = false;
    bool bLandHandsOnArrival = false;
    bool bTilesLit = false;
    bool bPawnSelected=false, bHeld=false, bLoopingBeam=false, bAimBox=false, bSpentHold=false;
    bool bTrackingBox=false;
    ECastPhase CastPhase=ECastPhase::Ready;
    float PhaseTime=0, HitTime=0;
    FVector AimPoint=FVector::ZeroVector;
    FHitResult AimHit;
    // Store contact samples in the receiver's space so the path follows a pushed box.
    struct FScarPoint
    {
        TWeakObjectPtr<UPrimitiveComponent> Surface;
        FVector Position, Normal;
        float Birth, Distance, Speed;
        bool bStart;
    };
    TArray<FScarPoint> Scars;
    TOptional<FScarPoint> LastScarSample;
    float ScarRemainder=0;
    bool bStrokeStarted=false;
    FIntVector TargetCell{-1,-1,0};
};

UCLASS(meta=(DisplayName="沦灭潭渊"))
class UGA_WaterAnchor : public UGameplayAbility
{
    GENERATED_BODY()
    void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* Info,
        const FGameplayAbilityActivationInfo Activation, const FGameplayEventData* Event) override;
};

UCLASS()
class UGA_Pulse : public UGameplayAbility
{
    GENERATED_BODY()
    void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* Info,
        const FGameplayAbilityActivationInfo Activation, const FGameplayEventData* Event) override;
};

UCLASS()
class UGA_Levitate : public UGameplayAbility
{
    GENERATED_BODY()
    void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* Info,
        const FGameplayAbilityActivationInfo Activation, const FGameplayEventData* Event) override;
};

UCLASS()
class RESONANTRUNES_API AShengLuGameMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    AShengLuGameMode();
    void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
    APawn* SpawnDefaultPawnAtTransform_Implementation(AController* NewPlayer, const FTransform& SpawnTransform) override;
#if WITH_EDITOR
    static void SetPIELevel(UPuzzleLevelAsset* Level);
#endif

private:
    void ConfigureBoard(AShengLuBoard& Board) const;
    UPROPERTY(Transient) TObjectPtr<UPuzzleLevelAsset> RequestedLevel;
    UPROPERTY(Transient) TObjectPtr<UPuzzleLevelAsset> NextLevelAsset;
    bool bFinalCampaignLevel = false;
};

UCLASS()
class UGA_Hiss : public UGameplayAbility
{
    GENERATED_BODY()
    void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* Info,
        const FGameplayAbilityActivationInfo Activation, const FGameplayEventData* Event) override;
};

