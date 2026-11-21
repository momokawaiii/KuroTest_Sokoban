#include "ShengLuGame.h"
#include "ShengLuHUD.h"
#include "AbilitySystemComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "Components/AudioComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Animation/AnimSequence.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/CommandLine.h"
#include "ProceduralMeshComponent.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "RuneBoxPawn.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
    constexpr float TravelTime = .34f;
#if WITH_EDITOR
    TWeakObjectPtr<UPuzzleLevelAsset> PIELevel;
#endif
    FLinearColor StoneTint(int X,int Y)
    {
        return (X+Y)%2 ? FLinearColor(.23f,.34f,.36f) : FLinearColor(.31f,.43f,.44f);
    }

    bool RespondToResult(AShengLuBoard& Board, ERuneHUDAction Action)
    {
        const auto* PC = Cast<APlayerController>(Board.GetController());
        auto* HUD = PC ? Cast<AShengLuHUD>(PC->GetHUD()) : nullptr;
        return HUD && HUD->RespondToResult(Action);
    }
}

AShengLuGameMode::AShengLuGameMode()
{
    DefaultPawnClass = AShengLuBoard::StaticClass();
    HUDClass = AShengLuHUD::StaticClass();
}

#if WITH_EDITOR
void AShengLuGameMode::SetPIELevel(UPuzzleLevelAsset* Level) { PIELevel = Level; }
#endif

void AShengLuGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
    Super::InitGame(MapName, Options, ErrorMessage);
    if (!ErrorMessage.IsEmpty()) return;
    const FString Option = UGameplayStatics::ParseOption(Options, TEXT("PuzzleLevel"));
#if WITH_EDITOR
    if (GetWorld()->WorldType == EWorldType::PIE && Option.IsEmpty()) RequestedLevel = PIELevel.Get();
#endif
    const bool bCampaign = UWorld::RemovePIEPrefix(GetWorld()->GetPackage()->GetName()) == UPuzzleLevelCatalog::PlayMap;
    if (RequestedLevel || bCampaign || !Option.IsEmpty())
    {
        FGuid Id;
        const auto* Catalog = LoadObject<UPuzzleLevelCatalog>(nullptr, UPuzzleLevelCatalog::AssetPath);
        if (!RequestedLevel && Catalog)
        {
            if (Option.IsEmpty()) RequestedLevel = Catalog->NextLevel();
            else if (FGuid::Parse(Option, Id)) RequestedLevel = Catalog->FindLevel(Id);
        }
        if (!RequestedLevel)
        {
            ErrorMessage = Option.IsEmpty() ? TEXT("Puzzle catalog has no starting level") :
                FString::Printf(TEXT("Puzzle level '%s' is missing from the catalog"), *Option);
            return;
        }
        if (Catalog)
        {
            NextLevelAsset = Catalog->NextLevel(RequestedLevel);
            bFinalCampaignLevel = !NextLevelAsset && Catalog->FindLevel(RequestedLevel->Definition.LevelId) == RequestedLevel;
        }
    }
    if (RequestedLevel)
        for (TActorIterator<AShengLuBoard> It(GetWorld()); It; ++It)
            ConfigureBoard(**It);
}

void AShengLuGameMode::ConfigureBoard(AShengLuBoard& Board) const
{
    Board.LevelAsset = RequestedLevel;
    Board.NextLevelAsset = NextLevelAsset;
    Board.bFinalCampaignLevel = bFinalCampaignLevel;
}

APawn* AShengLuGameMode::SpawnDefaultPawnAtTransform_Implementation(AController* NewPlayer, const FTransform& SpawnTransform)
{
    for (TActorIterator<AShengLuBoard> It(GetWorld()); It; ++It)
        if (!It->IsPlayerControlled()) return *It;
    if (RequestedLevel)
    {
        auto* Pawn = GetWorld()->SpawnActorDeferred<AShengLuBoard>(DefaultPawnClass, SpawnTransform,
            nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
        if (Pawn)
        {
            ConfigureBoard(*Pawn);
            Pawn->FinishSpawning(SpawnTransform);
        }
        return Pawn;
    }
    return Super::SpawnDefaultPawnAtTransform_Implementation(NewPlayer,SpawnTransform);
}
AShengLuBoard::AShengLuBoard()
{
    AutoPossessAI = EAutoPossessAI::Disabled;
    AIControllerClass = nullptr;
    PrimaryActorTick.bCanEverTick = true;
    RootComponent = CreateDefaultSubobject<USceneComponent>("BoardOrigin");
    Camera = CreateDefaultSubobject<UCameraComponent>("BoardCamera");
    Camera->SetupAttachment(RootComponent);
    const FVector Focus(ShengLuLayout::CellSize,ShengLuLayout::CellSize,.4f*ShengLuLayout::LayerHeight);
    const FVector Offset=FVector(-880,-690,860)*ShengLuLayout::Scale;
    Camera->SetRelativeLocation(Focus+Offset);
    Camera->SetRelativeRotation((-Offset).Rotation());
    Camera->SetProjectionMode(ECameraProjectionMode::Orthographic);
    Camera->OrthoWidth = 1350.f*ShengLuLayout::Scale;
    Camera->bConstrainAspectRatio = false;
    Camera->bOverrideAspectRatioAxisConstraint = true;
    Camera->SetAspectRatioAxisConstraint(AspectRatio_MaintainYFOV);
    Camera->PostProcessSettings.bOverride_MotionBlurAmount = true;
    Camera->PostProcessSettings.MotionBlurAmount = 0.f;
    Abilities = CreateDefaultSubobject<UAbilitySystemComponent>("Resonance");
    Hero = CreateDefaultSubobject<USkeletalMeshComponent>("Manny");
    Hero->SetupAttachment(RootComponent);
    Hero->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    static ConstructorHelpers::FObjectFinder<USkeletalMesh> Drongo(TEXT("/Game/ParagonDrongo/Characters/Heroes/Drongo/Meshes/Drongo_GDC"));
    Hero->SetSkeletalMeshAsset(Drongo.Object);
    static ConstructorHelpers::FObjectFinder<UAnimSequence> IdleAsset(TEXT("/Game/ParagonDrongo/Characters/Heroes/Drongo/Animations/Idle_NonCombat"));
    static ConstructorHelpers::FObjectFinder<UAnimSequence> WalkAsset(TEXT("/Game/ParagonDrongo/Characters/Heroes/Drongo/Animations/Jog_Fwd"));
    Idle = IdleAsset.Object; Walk = WalkAsset.Object;
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Block(TEXT("/Engine/BasicShapes/Cube"));
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> Mat(TEXT("/Game/Level/Element/M_Stone"));
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> Air(TEXT("/Game/VFX/Puzzle/M_Wind"));
    Cube = Block.Object; Stone = Mat.Object; WindMat = Air.Object;
    Ribbons = CreateDefaultSubobject<UProceduralMeshComponent>("ResonanceFX");
    Ribbons->SetupAttachment(RootComponent);
    Ribbons->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Ribbons->SetCastShadow(false);
    Ribbons->bUseAsOccluder = false;
    SetupBoardMarkers();
    for (int32 Y=0; Y<3; ++Y)
        for (int32 X=0; X<3; ++X)
        {
            Tiles.Add(Piece(*FString::Printf(TEXT("Tile_%d_%d"),X,Y),FVector(Y*ShengLuLayout::CellSize,X*ShengLuLayout::CellSize,0)-FVector(0,0,11)*ShengLuLayout::Scale,FVector(194,194,22)*ShengLuLayout::Scale,StoneTint(X,Y)));
        }
    Piece("Foundation",FVector(ShengLuLayout::CellSize,ShengLuLayout::CellSize,-32*ShengLuLayout::Scale),FVector(612,612,26)*ShengLuLayout::Scale,{0.064f,0.099f,0.11f});
    Piece("Plinth",FVector(ShengLuLayout::CellSize,2*ShengLuLayout::CellSize,ShengLuLayout::LayerHeight/2),{185*ShengLuLayout::Scale,185*ShengLuLayout::Scale,ShengLuLayout::LayerHeight},{0.30f,0.37f,0.39f});
    Piece("PlinthCap",FVector(ShengLuLayout::CellSize,2*ShengLuLayout::CellSize,ShengLuLayout::LayerHeight-4*ShengLuLayout::Scale),FVector(190,190,8)*ShengLuLayout::Scale,{0.53f,0.59f,0.55f});
    Crate = Piece("RuneA",FVector(ShengLuLayout::CellSize,ShengLuLayout::CellSize,ShengLuLayout::CrateSide*.5f),FVector(ShengLuLayout::CrateSide),{0.095f,0.46f,0.44f});
    Crate->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    Crate->SetCollisionResponseToAllChannels(ECR_Ignore);
    Crate->SetCollisionResponseToChannel(ECC_Visibility,ECR_Block);
    auto* Band = Piece("RuneInlay",FVector::ZeroVector,{107,107,6},{0.82f,0.64f,0.28f});
    Band->SetupAttachment(Crate);
    Band->SetRelativeScale3D(FVector(107.,107.,6.)/105.);
    Band->SetRelativeLocation(FVector(0,0,27./1.05));
    Hero->SetRelativeRotation(FRotator(0,-50,0));
    Ribbons->SetMaterial(0,WindMat);
    Tornado=CastSystem("NatureTornado",TEXT("Air/02"),TEXT("Air02_Tornado"));
    PlayerWind=CastSystem("PlayerWind",TEXT("Air/02"),TEXT("Air02_Range"));
    Lightning=CastSystem("NatureLightning",TEXT("Electric/01"),TEXT("Electric01_Trail"));
    PulseImpact=CastSystem("NaturePulseImpact",TEXT("Electric/01"),TEXT("Electric01_Hit"));
    SetupCasting();

}

FVector AShengLuBoard::Cell(FIntVector V) const
{
    const FVector Position = Definition.GridToWorld(V);
    return LevelAsset ? Position : GetActorTransform().TransformPosition(Position - Definition.GridOrigin);
}

UAbilitySystemComponent* AShengLuBoard::GetAbilitySystemComponent() const
{
    return Abilities;
}

FVector AShengLuBoard::BoxCenter(FIntVector V) const { return Cell(V)+FVector(0,0,Definition.BoxSide()*.5f); }

UStaticMeshComponent* AShengLuBoard::Piece(FName Name, FVector Location, FVector Size, FLinearColor Tint)
{
    auto* Mesh = CreateDefaultSubobject<UStaticMeshComponent>(Name);
    Mesh->ComponentTags.Add(TEXT("LegacyGeometry"));
    Mesh->SetupAttachment(RootComponent);
    Mesh->SetStaticMesh(Cube);
    Mesh->SetRelativeScale3D(Size/100.f);
    Mesh->SetRelativeLocation(Location);
    const bool BoardSurface=Name.ToString().StartsWith(TEXT("Tile_")) || Name==TEXT("Plinth") || Name==TEXT("PlinthCap");
    const bool BeamSurface=BoardSurface || Name==TEXT("Foundation") || Name==TEXT("RuneInlay");
    Mesh->SetCollisionEnabled(BeamSurface ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
    Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
    Mesh->SetCollisionResponseToChannel(ECC_Visibility,ECR_Block);
    if (BoardSurface) Mesh->ComponentTags.Add(TEXT("BoardSurface"));
    Mesh->SetMaterial(0,Stone);
    Mesh->SetDefaultCustomPrimitiveDataVector4(0,FVector4(Tint.R,Tint.G,Tint.B,1));
    return Mesh;
}

void AShengLuBoard::BeginPlay()
{
    Super::BeginPlay();
    // Sample the raised hand only after its pose and socket children are current.
    AddTickPrerequisiteComponent(Hands);
    for (const auto& Pair : EffectAssets)
    {
        auto* Component=CastChecked<UNiagaraComponent>(GetDefaultSubobjectByName(Pair.Key));
        Component->SetAsset(Pair.Value.LoadSynchronous());
    }
    Lightning->SetSystemFixedBounds(FBox(FVector(-650*ShengLuLayout::Scale),FVector(650*ShengLuLayout::Scale)));
    Definition = LevelAsset ? LevelAsset->Definition : FPuzzleLevelDefinition::MakeShengLu(GetActorLocation());
    if (const auto Errors = FPuzzleSolver::Validate(Definition); !Errors.IsEmpty())
    {
        Notice = FString::Join(Errors, TEXT(" / "));
        UE_LOG(LogTemp, Error, TEXT("Invalid puzzle level: %s"), *Notice);
        return;
    }
    State = FPuzzleRuntimeState::FromDefinition(Definition);
    LegacyView = FShengLuRules::View(State);
    if (LevelAsset) BuildAssetPresentation();
    if (!SpawnBoxPawns()) return;
    RestoreVisuals();
    Abilities->InitAbilityActorInfo(this,this);
    const auto Grant = [this](ERuneAction Action, TSubclassOf<UGameplayAbility> Ability)
    {
        if (RuneSkills::IsUnlocked(Definition, Action)) Abilities->GiveAbility(FGameplayAbilitySpec(Ability, 1));
    };
    Grant(ERuneAction::Pulse, UGA_Pulse::StaticClass());
    Grant(ERuneAction::Levitate, UGA_Levitate::StaticClass());
    Grant(ERuneAction::WaterAnchor, UGA_WaterAnchor::StaticClass());
    Grant(ERuneAction::Hiss, UGA_Hiss::StaticClass());
    bPuzzleReady = true;
}

void AShengLuBoard::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (EndPlayReason == EEndPlayReason::Destroyed)
        for (const auto& Entry : BoxPawns)
            if (IsValid(Entry.Value)) Entry.Value->Destroy();
    Super::EndPlay(EndPlayReason);
}

void AShengLuBoard::SetupPlayerInputComponent(UInputComponent* Input)
{
    Super::SetupPlayerInputComponent(Input);
    Input->BindKey(EKeys::W,IE_Pressed,this,&AShengLuBoard::MoveForward);
    Input->BindKey(EKeys::S,IE_Pressed,this,&AShengLuBoard::MoveBackward);
    Input->BindKey(EKeys::A,IE_Pressed,this,&AShengLuBoard::MoveLeft);
    Input->BindKey(EKeys::D,IE_Pressed,this,&AShengLuBoard::MoveRight);
    Input->BindKey(EKeys::One,IE_Pressed,this,&AShengLuBoard::SelectPulse);
    Input->BindKey(EKeys::Two,IE_Pressed,this,&AShengLuBoard::SelectWind);
    Input->BindKey(EKeys::Three,IE_Pressed,this,&AShengLuBoard::SelectWater);
    Input->BindKey(EKeys::Four,IE_Pressed,this,&AShengLuBoard::SelectHiss);
    Input->BindKey(EKeys::G,IE_Pressed,this,&AShengLuBoard::DispelPlayerWind);
    Input->BindKey(EKeys::Tab,IE_Pressed,this,&AShengLuBoard::CycleAimBox);
    Input->BindKey(EKeys::F,IE_Pressed,this,&AShengLuBoard::ExitFirstPerson);
    Input->BindKey(EKeys::SpaceBar,IE_Pressed,this,&AShengLuBoard::SelectPlayerCell);
    Input->BindKey(EKeys::LeftMouseButton,IE_Pressed,this,&AShengLuBoard::ClickBoard);
    Input->BindKey(EKeys::RightMouseButton,IE_Pressed,this,&AShengLuBoard::Cancel);
    Input->BindKey(EKeys::LeftMouseButton,IE_Released,this,&AShengLuBoard::ReleaseClick);
    Input->BindKey(EKeys::Escape,IE_Pressed,this,&AShengLuBoard::CancelFromInput);
    Input->BindAxisKey(EKeys::MouseX,this,&AShengLuBoard::MouseX);
    Input->BindAxisKey(EKeys::MouseY,this,&AShengLuBoard::MouseY);
    Input->BindAxisKey(EKeys::MouseWheelAxis,this,&AShengLuBoard::ZoomOverview);
    Input->BindKey(EKeys::Z,IE_Pressed,this,&AShengLuBoard::UndoFromInput);
    Input->BindKey(EKeys::R,IE_Pressed,this,&AShengLuBoard::RetryFromInput);
    Input->BindKey(EKeys::Enter,IE_Pressed,this,&AShengLuBoard::AcceptResultFromInput);
    SetView(Selected.IsSet());
}

bool AShengLuBoard::IsLevelCleared() const
{
    return bPuzzleReady && !bTravelRequested && !IsAnimating() && !WaterInFlight() && IsRestored();
}

bool AShengLuBoard::CanAdvanceLevel() const
{
    return NextLevelAsset && IsLevelCleared();
}

bool AShengLuBoard::CanFinishCampaign() const
{
    return bFinalCampaignLevel && IsLevelCleared();
}

bool AShengLuBoard::ShouldShowCampaignVictory() const
{
    return !bCampaignVictoryDismissed && CanFinishCampaign();
}

void AShengLuBoard::AdvanceLevel()
{
    if (!CanAdvanceLevel()) return;
    if (const auto Errors = FPuzzleSolver::Validate(NextLevelAsset->Definition); !Errors.IsEmpty())
    {
        Notice = TEXT("下一关校验失败 · ") + Errors[0];
        return;
    }
    bTravelRequested = true;
    UGameplayStatics::OpenLevel(this, FName(UPuzzleLevelCatalog::PlayMap), true,
        TEXT("PuzzleLevel=") + NextLevelAsset->Definition.LevelId.ToString());
}

void AShengLuBoard::FinishCampaign()
{
    if (ShouldShowCampaignVictory())
        UKismetSystemLibrary::QuitGame(this, Cast<APlayerController>(Controller), EQuitPreference::Quit, false);
}

void AShengLuBoard::DismissCampaignVictory()
{
    if (ShouldShowCampaignVictory()) bCampaignVictoryDismissed = true;
}

void AShengLuBoard::AcceptResultFromInput()
{
    if (RespondToResult(*this, ERuneHUDAction::Accept)) return;
    if (ShouldShowCampaignVictory()) FinishCampaign();
    else if (CanFinishCampaign()) bCampaignVictoryDismissed = false;
    else AdvanceLevel();
}

void AShengLuBoard::RetryFromInput()
{
    if (!RespondToResult(*this, ERuneHUDAction::Retry)) ResetPuzzle();
}

void AShengLuBoard::UndoFromInput()
{
    if (!RespondToResult(*this, ERuneHUDAction::Undo)) Undo();
}

void AShengLuBoard::CancelFromInput()
{
    if (!RespondToResult(*this, ERuneHUDAction::Inspect)) Cancel();
}

void AShengLuBoard::MoveForward() { MoveFromInput(ERuneAction::North); }
void AShengLuBoard::MoveBackward() { MoveFromInput(ERuneAction::South); }
void AShengLuBoard::MoveLeft() { MoveFromInput(ERuneAction::West); }
void AShengLuBoard::MoveRight() { MoveFromInput(ERuneAction::East); }
void AShengLuBoard::SelectWind() { SelectSkill(ERuneAction::Levitate); }
void AShengLuBoard::SelectWater() { SelectSkill(ERuneAction::WaterAnchor); }
void AShengLuBoard::SelectPulse() { SelectSkill(ERuneAction::Pulse); }
void AShengLuBoard::SelectHiss() { SelectSkill(ERuneAction::Hiss); }

void UGA_Levitate::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* Info,
    const FGameplayAbilityActivationInfo Activation, const FGameplayEventData*)
{
    auto* Board=Cast<AShengLuBoard>(Info->AvatarActor.Get());
    const bool bApplied = Board && Board->Apply(ERuneAction::Levitate);
    EndAbility(Handle,Info,Activation,false,!bApplied);
}

void UGA_Pulse::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* Info,
    const FGameplayAbilityActivationInfo Activation, const FGameplayEventData*)
{
    auto* Board=Cast<AShengLuBoard>(Info->AvatarActor.Get());
    const bool bApplied = Board && Board->Apply(ERuneAction::Pulse);
    EndAbility(Handle,Info,Activation,false,!bApplied);
}

void UGA_Hiss::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* Info,
    const FGameplayAbilityActivationInfo Activation, const FGameplayEventData*)
{
    auto* Board = Cast<AShengLuBoard>(Info->AvatarActor.Get());
    const bool bApplied = Board && Board->Apply(ERuneAction::Hiss);
    EndAbility(Handle, Info, Activation, false, !bApplied);
}

bool AShengLuBoard::Apply(ERuneAction Action)
{
    return TryPuzzleAction(MakeAction(Action));
}

FIntVector AShengLuBoard::PulseDirection() const
{
    if (const auto* Box = State.FindBox(TargetBoxId); IsFirstPerson() && Box)
        return FPuzzleSolver::PushFromLook(State.PlayerPosition, Box->GridPosition, ToGridDirection(Eyes->GetForwardVector()));
    return {State.Facing.X, State.Facing.Y, 0};
}

FPuzzleAction AShengLuBoard::MakeAction(ERuneAction Action) const
{
    if (Action == ERuneAction::Levitate && AimedAtSelf()) return {ERuneAction::LevitateSelf};
    const int32 Id = UsesLegacyPresentation() && !State.Boxes.IsEmpty() ? State.Boxes[0].BoxId : TargetBoxId;
    return {Action, Id, Action == ERuneAction::Pulse ? PulseDirection() : FIntVector::ZeroValue,
        Action == ERuneAction::Hiss && AimedAtObstacle() ? TargetCell : FPuzzleLevelDefinition::InvalidPosition};
}

FPuzzleActionResult AShengLuBoard::Evaluate(FPuzzleAction Action) const
{
    return FPuzzleSolver::Evaluate(Definition, State, Action);
}

bool AShengLuBoard::TryPuzzleAction(FPuzzleAction Action)
{
    if (!bPuzzleReady || bTravelRequested || IsAnimating() || WaterInFlight()) return false;
    auto Result = Evaluate(Action);
    if (!Result.IsValid()) { Notice=Result.Reason; return false; }
    const FVector WindOrigin = Action.Action == ERuneAction::Levitate ?
        Cell(SurfaceUnder(State.FindBox(Action.BoxId)->GridPosition)) : FVector::ZeroVector;
    const bool bWindLanding = State.PlayerWindTurns > 0 && FPuzzleSolver::Direction(Action.Action) != FIntPoint::ZeroValue;
    bLandHandsOnArrival = State.PlayerWindTurns > 0 && Result.After.PlayerPosition.Z < State.PlayerPosition.Z;
    History.Add({State, WindFoot, WindAge, WindFadeAge, WindBoxId});
    State = MoveTemp(Result.After);
    LegacyView = FShengLuRules::View(State);
    if (bWindLanding) bPawnSelected = false;
    SyncPlayerWindState();
    // The water launch already produced confirmation feedback before the anchor landed.
    if (Action.Action != ERuneAction::WaterAnchor || WaterAge < 0.f) RecordConfirmedSkill(Action.Action);
    if (Action.Action == ERuneAction::Levitate) BeginWind(WindOrigin, Action.BoxId);
    if (Action.Action == ERuneAction::LevitateSelf) PlayHandTransition(JumpStartHands);
    if (Action.Action == ERuneAction::Hiss && IsFirstPerson() && Selected == ERuneAction::Hiss)
    {
        PlayHandTransition(GetCastClip(ECastPhase::End));
        PlayHandPhase(ECastPhase::Ready);
    }
    Notice.Reset();
    HeroFrom=Hero->GetComponentLocation();
    HeroTo=Cell(State.PlayerPosition);
    UpdateBoardPresentation(false);
    BoxMotions.Reset();
    for (const auto& Box : State.Boxes)
    {
        auto* Mesh = BoxMesh(Box.BoxId);
        if (!Mesh) continue;
        const auto Center = [&](FIntVector Position)
        {
            return LevelAsset ? Definition.BoxBounds(Position, Box.bHiss).GetCenter() : BoxCenter(Position);
        };
        const FVector From = Mesh->GetComponentLocation(), To = Center(Box.GridPosition);
        const FVector Via = Action.BoxId == Box.BoxId && Definition.IsValidGridCoordinate(Result.BeforeGravity) ?
            Center(Result.BeforeGravity) : From;
        if (!From.Equals(To, 1) || !From.Equals(Via, 1)) BoxMotions.Add({Box.BoxId, From, Via, To});
    }
    bWalking=FVector::DistSquared2D(HeroFrom,HeroTo)>1.;
    if (bWalking)
    {
        if (IsFirstPerson()) ResetCast(Selected);
        Hero->SetWorldRotation((HeroTo-HeroFrom).Rotation()+FRotator(0,-90,0));
        Hero->PlayAnimation(Walk,true);
    }
    MotionRemaining=TravelTime;
    return true;
}

void AShengLuBoard::RecordConfirmedSkill(ERuneAction Skill)
{
    const int32 Slot = RuneSkills::Slot(Skill == ERuneAction::LevitateSelf ? ERuneAction::Levitate : Skill);
    if (Slot == 0) return;
    LastConfirmedSkillSlot = Slot;
    LastSkillConfirmedTime = GetWorld()->GetRealTimeSeconds();
}

void AShengLuBoard::Tick(float Dt)
{
    Super::Tick(Dt);
    if (!bPuzzleReady) return;
    UpdateBoardCamera(Dt);
    if (IsAnimating())
    {
        MotionRemaining=FMath::Max(0.f,MotionRemaining-Dt);
        const float T=1.f-MotionRemaining/TravelTime;
        const FVector HeroPosition = FMath::Lerp(HeroFrom, HeroTo, FMath::SmoothStep(0.f, 1.f, T));
        if (IsFirstPerson())
        {
            const FVector ViewMotion = HeroPosition - Hero->GetComponentLocation();
            if (!ViewMotion.IsNearlyZero()) Eyes->AddWorldOffset(ViewMotion);
        }
        Hero->SetWorldLocation(HeroPosition);
        for (const auto& Motion : BoxMotions)
        {
            FVector Visual;
            if (Motion.Via.Equals(Motion.To,1)) Visual=FMath::Lerp(Motion.From,Motion.To,FMath::SmoothStep(0.f,1.f,T));
            else if (T<.5f) Visual=FMath::Lerp(Motion.From,Motion.Via,T*2);
            else Visual=FMath::Lerp(Motion.Via,Motion.To,FMath::Square((T-.5f)*2));
            if (auto* Box = BoxPawns.FindRef(Motion.BoxId).Get()) Box->SetActorLocation(Visual);
        }
        if (!IsAnimating())
        {
            if (bWalking) { Hero->PlayAnimation(Idle,true); bWalking=false; }
            if (bLandHandsOnArrival)
            {
                bLandHandsOnArrival = false;
                PlayHandTransition(JumpEndHands);
            }
        }
    }
    if (!LevelAsset && (bPawnSelected || bTilesLit))
    {
        const auto Hover=HoverCell();
        TArray<FIntVector, TInlineAllocator<4>> Steps;
        if (bPawnSelected)
            for (ERuneAction Step : {ERuneAction::North, ERuneAction::East, ERuneAction::South, ERuneAction::West})
                if (auto Destination = StepDestination(Step)) Steps.Add(*Destination);
        for (int I=0;I<Tiles.Num();++I)
        {
            const FIntVector P(I%3,I/3,0);
            FLinearColor C=StoneTint(P.X,P.Y);
            if (Steps.Contains(P))
                C=Hover==P ? FLinearColor(.78f,.48f,.10f) : FLinearColor(.06f,.58f,.42f);
            Tiles[I]->SetCustomPrimitiveDataVector4(0,FVector4(C.R,C.G,C.B,1));
        }
        bTilesLit=bPawnSelected;
    }
    UpdateCasting(Dt);
    UpdateWater(Dt);
    DrawEffects(Dt);
    UpdateBoardMarkers();
    UpdatePointerMode();
}

void AShengLuBoard::RestoreVisuals(const FActionSnapshot* Snapshot)
{
    bCampaignVictoryDismissed = false;
    LastConfirmedSkillSlot = 0;
    ClearWater();
    bHandTransitionPlaying = false;
    bLandHandsOnArrival = false;
    ResetCast(Selected);
    CastTransient->Stop();
    MotionRemaining=0; bWalking=false;
    Lightning->DeactivateImmediate();
    BeamArm->DeactivateImmediate();
    BeamFadeUntil=-1;
    FadeContact();
    for (const auto& Contact : FadingContacts)
        if (auto* FX=Contact.FX.Get()) FX->DestroyComponent();
    FadingContacts.Reset();
    ContactPuffs.Reset();
    ContactArcs.Reset();
    BranchSerial=0;
    PuffSerial=0;
    ClearWind();
    if (Snapshot && Snapshot->WindAge>=0.f)
    {
        BeginWind(Snapshot->WindFoot,Snapshot->WindBoxId,Snapshot->WindAge);
        WindFadeAge=Snapshot->WindFadeAge;
        if (WindFadeAge>=0.f)
        {
            Tornado->SetVariableBool(TEXT("User.IsActive"),false);
            WindLoop->Stop();
        }
        UpdateWind(0.f);
    }
    Notice.Reset();
    Ribbons->ClearAllMeshSections();
    Scars.Reset();
    Hero->SetWorldLocation(Cell(State.PlayerPosition));
    Hero->SetWorldRotation(WorldFacing().Rotation()+FRotator(0,-90,0));
    Hero->PlayAnimation(Idle,true);
    UpdateBoardPresentation(true);
    SyncPlayerWindState();
    if (IsFirstPerson())
    {
        UpdateLook(true);
        UpdateAim();
    }
}

void AShengLuBoard::Undo()
{
    if (!bPuzzleReady || bTravelRequested) return;
    if (WaterInFlight()) { ClearWater(); Cancel(); return; }
    if (!History.IsEmpty())
    {
        const auto Snapshot=History.Pop();
        State=Snapshot.Board;
        LegacyView = FShengLuRules::View(State);
        RestoreVisuals(&Snapshot);
    }
    else Cancel();
}

void AShengLuBoard::ResetPuzzle()
{
    if (!bPuzzleReady || bTravelRequested) return;
    History.Reset();
    State = FPuzzleRuntimeState::FromDefinition(Definition);
    LegacyView = FShengLuRules::View(State);
    RestoreVisuals();
}
void AShengLuBoard::Cancel()
{
    bHandTransitionPlaying = false;
    ResetCast(Selected);
    CastTransient->Stop();
    Notice.Reset();
}

