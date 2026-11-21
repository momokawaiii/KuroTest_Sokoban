#include "ShengLuGame.h"
#include "ShengLuHUD.h"
#include "RuneBoxPawn.h"
#include "AbilitySystemComponent.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/AudioComponent.h"
#include "Components/DecalComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CurveTable.h"
#include "GameFramework/PlayerController.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "ProceduralMeshComponent.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
    constexpr ERuneAction Steps[]={ERuneAction::North, ERuneAction::East, ERuneAction::South, ERuneAction::West};
    constexpr float LookDegreesPerUnit = .60f;
}

void AShengLuBoard::SetupCasting()
{
    SetupCastAudio();
    HeroPick=CreateDefaultSubobject<UCapsuleComponent>("MannyPick");
    HeroPick->SetupAttachment(Hero);
    HeroPick->SetRelativeLocation(FVector(0,0,87));
    HeroPick->InitCapsuleSize(39,88);
    HeroPick->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    HeroPick->SetCollisionResponseToAllChannels(ECR_Ignore);
    HeroPick->SetCollisionResponseToChannel(ECC_Visibility,ECR_Block);
    Eyes=CreateDefaultSubobject<UCameraComponent>("CastCamera");
    Eyes->SetupAttachment(RootComponent);
    Eyes->bAutoActivate=false;
    Eyes->bConstrainAspectRatio=false;
    Eyes->FieldOfView=88;
    Eyes->PostProcessSettings=Camera->PostProcessSettings;
    Eyes->PostProcessSettings.bOverride_MotionBlurAmount=true;
    Eyes->PostProcessSettings.MotionBlurAmount=0;
    HandGlow=CreateDefaultSubobject<UPointLightComponent>("ElectricHandLight");
    HandGlow->SetupAttachment(RootComponent);
    HandGlow->SetMobility(EComponentMobility::Movable);
    HandGlow->SetCastShadows(false);
    HandGlow->SetLightColor(FLinearColor(.36f,.46f,1.f));
    HandGlow->SetIntensityUnits(ELightUnits::Candelas);
    HandGlow->SetAttenuationRadius(65.f);
    HandGlow->SetVisibility(false);
    ContactGlow=CreateDefaultSubobject<UPointLightComponent>("ElectricContactLight");
    ContactGlow->SetupAttachment(RootComponent);
    ContactGlow->SetCastShadows(false);
    ContactGlow->SetIntensityUnits(ELightUnits::Candelas);
    ContactGlow->SetLightColor(FLinearColor(.23f,.12f,1.f));
    ContactGlow->SetAttenuationRadius(95.f);
    ContactGlow->SetVisibility(false);
    Hands=CreateDefaultSubobject<USkeletalMeshComponent>("CastHands");
    Hands->SetupAttachment(Eyes);
    Hands->SetRelativeLocationAndRotation(FVector(-5,0,-161),FRotator(0,-90,0));
    Hands->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Hands->SetCastShadow(false);
    Hands->bPerBoneMotionBlur=true;
    Hands->VisibilityBasedAnimTickOption=EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
    const FString Folder = TEXT("/Game/Character/FirstPerson/SM_FirstPerson/SkeletalMeshes/");
    const FString Base = Folder + TEXT("SM_FirstPerson");
    ConstructorHelpers::FObjectFinder<USkeletalMesh> Model(*Base);
    Hands->SetSkeletalMeshAsset(Model.Object);
    Hands->SetVisibility(false);
    for (const TCHAR* Type : {TEXT("2h_Beam_"),TEXT("R_Beam_")})
        for (const TCHAR* Stage : {TEXT("Start"),TEXT("Loop"),TEXT("End")})
        {
            ConstructorHelpers::FObjectFinder<UAnimSequence> Clip(*(Base+Type+Stage+TEXT("_Retarget")));
            CastClips.Add(Clip.Object);
        }
    const auto LoadHandClip = [&Folder](const TCHAR* Name) -> UAnimSequence*
    {
        ConstructorHelpers::FObjectFinder<UAnimSequence> Clip(*(Folder + Name + TEXT("_Retarget")));
        return Clip.Object.Get();
    };
    EquipHands = LoadHandClip(TEXT("Equip"));
    JumpStartHands = LoadHandClip(TEXT("Jump_Start"));
    JumpLoopHands = LoadHandClip(TEXT("Jump_Loop"));
    JumpEndHands = LoadHandClip(TEXT("Jump_End"));
    for (const TCHAR* Name : {TEXT("2h_Orb_Start"), TEXT("2h_Orb_Loop"), TEXT("2h_Orb_End")})
        HissClips.Add(LoadHandClip(Name));
    ConstructorHelpers::FObjectFinder<UMaterialInterface> Discharge(TEXT("/Game/VFX/Puzzle/M_PulseDischarge"));
    ConstructorHelpers::FObjectFinder<UMaterialInterface> Scar(TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/Electric/01/Materials/M_Player_Skill_Electric01_RibbonDecal"));
    ConstructorHelpers::FObjectFinder<UCurveTable> Curves(TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/Electric/01/Niagara/NS_Player_Skill_Electric01_Hit_BakedCurves"));
    Ribbons->SetMaterial(1,Discharge.Object);
    Ribbons->SetMaterial(2,Scar.Object);
    ConstructorHelpers::FObjectFinder<UMaterialInterface> PalmWave(TEXT("/Game/VFX/Puzzle/M_Electric01_PalmWave"));
    Ribbons->SetMaterial(3,PalmWave.Object);
    Ribbons->SetMaterial(4,PalmWave.Object);
    HitCurves=Curves.Object;
    ConstructorHelpers::FObjectFinder<UCurveTable> PalmCurves(TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/Electric/01/Niagara/NS_Player_Skill_Electric01_Charge_BakedCurves"));
    ChargeCurves=PalmCurves.Object;
    ConstructorHelpers::FObjectFinder<UCurveTable> BeamCurves(TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/Electric/01/Niagara/NS_Player_Skill_Electric01_Trail_BakedCurves"));
    TrailCurves=BeamCurves.Object;
    ConstructorHelpers::FObjectFinder<UCurveTable> ForearmCurves(TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/Electric/01/Niagara/NS_Player_Skill_Electric01_Arm_BakedCurves"));
    ArmCurves=ForearmCurves.Object;
    Range=CastSystem("WindRange",TEXT("Air/02"),TEXT("Air02_Range"));
    SetupWater();
    Charge=CastSystem("PulseCharge",TEXT("Electric/01"),TEXT("Electric01_Charge"));
    ChargeArm=CastSystem("PulseChargeArm",TEXT("Electric/01"),TEXT("Electric01_Charge_Arm"));
    BeamArm=CastSystem("PulseArm",TEXT("Electric/01"),TEXT("Electric01_Arm"));
    BeamEnd=CastSystem("PulseEnd",TEXT("Electric/01"),TEXT("Electric01_End"));
    // The imported mesh contains the parent bones. These are the original socket transforms.
    for (auto* FX : {Charge.Get(), Lightning.Get(), BeamEnd.Get()})
    {
        FX->SetupAttachment(Hands,TEXT("Weapon_r_end"));
        FX->SetRelativeLocationAndRotation(FVector(3.91368,6.18725,3.63716),FRotator(33.538,11.978,11.762));
    }
    for (auto* FX : {ChargeArm.Get(), BeamArm.Get()})
    {
        FX->SetupAttachment(Hands,TEXT("lowerarm_r"));
        FX->SetRelativeRotation(FRotator(.039,-179.927,90.022));
    }
}

UNiagaraComponent* AShengLuBoard::CastSystem(FName Name,const TCHAR* Family,const TCHAR* Effect)
{
    auto* Component=CreateDefaultSubobject<UNiagaraComponent>(Name);
    Component->SetupAttachment(RootComponent);
    Component->SetAutoActivate(false);
    const FString Asset=FString(TEXT("NS_Player_Skill_"))+Effect;
    const FString Path=FString::Printf(TEXT("/Game/VFX/CodiVFX/PlayerVFX/Skills/%s/Niagara/%s.%s"),Family,*Asset,*Asset);
    EffectAssets.Add(Name,TSoftObjectPtr<UNiagaraSystem>(FSoftObjectPath(Path)));
    return Component;
}

bool AShengLuBoard::IsFirstPerson() const
{
    return Eyes->IsActive();
}

void AShengLuBoard::SetView(bool FirstPerson)
{
    const bool bEntering = FirstPerson && !IsFirstPerson();
    Camera->SetActive(!FirstPerson);
    Eyes->SetActive(FirstPerson);
    Hero->SetVisibility(!FirstPerson);
    HeroPick->SetCollisionEnabled(FirstPerson?ECollisionEnabled::NoCollision:ECollisionEnabled::QueryOnly);
    Hands->SetVisibility(FirstPerson);
    if (bEntering) PlayHandTransition(EquipHands);
    UpdatePointerMode(true);
}

void AShengLuBoard::UpdatePointerMode(bool bForce)
{
    auto* PC=Cast<APlayerController>(Controller);
    if (!PC) return;
    // Settlement frees the pointer while keeping the camera and hands in their current view.
    const bool bShowCursor = !IsFirstPerson() || IsShowingResult();
    if (!bForce && PC->bShowMouseCursor == bShowCursor) return;
    PC->bShowMouseCursor = bShowCursor;
    if (!bShowCursor)
    {
        FInputModeGameOnly Mode;
        Mode.SetConsumeCaptureMouseDown(false);
        PC->SetInputMode(Mode);
    }
    else
    {
        FInputModeGameAndUI Mode;
        Mode.SetHideCursorDuringCapture(false);
        PC->SetInputMode(Mode);
    }
}

void AShengLuBoard::ExitFirstPerson()
{
    if (!IsFirstPerson()) return;
    bHandTransitionPlaying = false;
    ResetCast({});
    CastTransient->Stop();
    Notice.Reset();
    SetView(false);
}

void AShengLuBoard::Turn(ERuneAction Direction)
{
    if (!bPuzzleReady || IsRestored() || IsAnimating()) return;
    State.Facing=FPuzzleSolver::Direction(Direction);
    LegacyView.Facing = State.Facing;
    Hero->SetWorldRotation(WorldFacing().Rotation()+FRotator(0,-90,0));
    if (Selected) { bTrackingBox=false; HitTime=0; UpdateLook(false); UpdateAim(); }
}

FVector AShengLuBoard::ToGridDirection(FVector Direction) const
{
    if (!LevelAsset) Direction = GetActorTransform().InverseTransformVector(Direction);
    if (Definition.bSwapXY) Swap(Direction.X, Direction.Y);
    return Direction;
}

void AShengLuBoard::MoveFromInput(ERuneAction Direction)
{
    if (IsPlayerLevitating()) return;
    if (!IsFirstPerson()) { Turn(Direction); return; }
    const FIntPoint Input = FPuzzleSolver::Direction(Direction);
    const FVector Step = ToGridDirection(Eyes->GetForwardVector() * Input.Y + Eyes->GetRightVector() * Input.X);
    const ERuneAction Move = FMath::Abs(Step.X) >= FMath::Abs(Step.Y)
        ? (Step.X >= 0 ? ERuneAction::East : ERuneAction::West)
        : (Step.Y >= 0 ? ERuneAction::North : ERuneAction::South);
    TryPuzzleAction({Move});
}

void AShengLuBoard::DispelPlayerWind()
{
    if (State.PlayerWindTurns > 0 && TryPuzzleAction({ERuneAction::ClearPlayerWind})) ResetCast(Selected);
}

void AShengLuBoard::UpdateLook(bool FrameTarget)
{
    const FVector Facing = WorldFacing();
    if (!UsesLegacyPresentation())
    {
        const float Scale = FMath::Min(1.f, FMath::Min(Definition.GridCellSize, Definition.LayerHeight()) / 200.f);
        const FVector Eye = Cell(State.PlayerPosition) + FVector(0, 0, 150.f * Scale) - Facing * Definition.GridCellSize * .22f;
        FVector Aim = Eye + Facing * Definition.GridCellSize * 2.f - FVector(0, 0, 60.f * Scale);
        if (FrameTarget)
        {
            double BestDistance = TNumericLimits<double>::Max();
            for (const auto& Box : State.Boxes)
            {
                const FVector Center = BoxMesh(Box.BoxId)->GetComponentLocation();
                const double Distance = FVector::DistSquared(Eye, Center);
                if (Distance >= BestDistance) continue;
                BestDistance = Distance;
                Aim = Center;
            }
        }
        Eyes->SetFieldOfView(92.f);
        Eyes->SetWorldLocationAndRotation(Eye, (Aim - Eye).Rotation());
        return;
    }
    FVector Eye=Cell(State.PlayerPosition)+FVector(0,0,154);
    const FIntPoint Delta(LegacyView.Box.X-State.PlayerPosition.X,LegacyView.Box.Y-State.PlayerPosition.Y);
    if (Selected==ERuneAction::Levitate || Selected==ERuneAction::WaterAnchor)
    {
        const FVector ToBox=(Crate->GetComponentLocation()-Eye).GetSafeNormal2D();
        // Pull back within the player's cell to keep a close box in view.
        Eye-=ToBox*42.f;
        Eyes->SetFieldOfView(Selected==ERuneAction::WaterAnchor?90.f:100.f);
    }
    else
    {
        Eye+=Facing*(Delta==State.Facing?35.f:-42.f);
        Eyes->SetFieldOfView(88.f);
    }
    const bool InFront=Delta==State.Facing || Delta==State.Facing*2;
    const FVector Target=(FrameTarget && (Selected==ERuneAction::Levitate || Selected==ERuneAction::WaterAnchor)) || InFront ?
        Crate->GetComponentLocation()+FVector(0,0,Selected==ERuneAction::Levitate?33.f*ShengLuLayout::Scale:0.f) : Eye+Facing*320-FVector(0,0,34);
    Eyes->SetWorldLocationAndRotation(Eye,(Target-Eye).Rotation());
}

void AShengLuBoard::MouseX(float Delta)
{
    if (IsFirstPerson() && Delta!=0 && !IsShowingResult())
        Eyes->AddWorldRotation(FRotator(0,Delta*LookDegreesPerUnit,0));
}

void AShengLuBoard::MouseY(float Delta)
{
    if (!IsFirstPerson() || Delta==0 || IsShowingResult()) return;
    FRotator Look=Eyes->GetComponentRotation();
    Look.Pitch=FMath::ClampAngle(Look.Pitch+Delta*LookDegreesPerUnit,-78.f,78.f);
    Eyes->SetWorldRotation(Look);
}

TOptional<FIntVector> AShengLuBoard::StepDestination(ERuneAction Action) const
{
    const auto Result = Evaluate({Action});
    if (Result.IsValid()) return Result.After.PlayerPosition;
    return {};
}

bool AShengLuBoard::CanStepTo(FIntPoint P) const
{
    for (ERuneAction Step : Steps)
        if (StepDestination(Step) == FIntVector(P.X, P.Y, 0)) return true;
    return false;
}

TOptional<FIntVector> AShengLuBoard::HoverCell() const
{
    auto* PC=Cast<APlayerController>(Controller);
    FHitResult Hit;
    if (PC && bPawnSelected && PC->GetHitResultUnderCursor(ECC_Visibility,false,Hit)) return PickTile(Hit);
    return {};
}

TOptional<FIntVector> AShengLuBoard::InspectCellAt(FVector2D ScreenPosition) const
{
    const auto* PC = Cast<APlayerController>(Controller);
    if (!PC || !bPuzzleReady || IsFirstPerson() || IsAnimating() || IsShowingResult()) return {};

    FHitResult Hit;
    const bool bHit = PC->GetHitResultAtScreenPosition(ScreenPosition, ECC_Visibility, false, Hit);
    const auto* CratePawn = Cast<ARuneBoxPawn>(Hit.GetActor());
    const auto* Box = CratePawn && CratePawn->GetOwner() == this ? State.FindBox(CratePawn->GetBoxId()) : nullptr;
    const bool bLegacyCrate = !LevelAsset && Hit.GetComponent() &&
        (Hit.GetComponent() == Crate || Hit.GetComponent()->IsAttachedTo(Crate));
    if (Box || bLegacyCrate)
    {
        const FIntVector Position = Box ? Box->GridPosition : LegacyView.Box;
        if (Definition.Goals.ContainsByPredicate([&](const FRuneGoalData& Goal) { return Goal.GridPosition == Position; }))
            return Position;
    }

    // Intersect the goal plane in both depth modes, including goals without a floor collider.
    FVector Origin, Direction;
    if (PC->DeprojectScreenPositionToWorld(ScreenPosition.X, ScreenPosition.Y, Origin, Direction) &&
        !FMath::IsNearlyZero(Direction.Z))
    {
        double Nearest = bHit && !bDepthViewActive ? Hit.Distance : TNumericLimits<double>::Max();
        TOptional<FIntVector> GoalPosition;
        for (const auto& Goal : Definition.Goals)
        {
            const FVector At = GoalMarkerCenter(Goal.GridPosition);
            const double Distance = (At.Z - Origin.Z) / Direction.Z;
            if (Distance < 0. || Distance >= Nearest) continue;
            const FVector Offset = Origin + Direction * Distance - At;
            const FVector Local = LevelAsset ? Offset : GetActorTransform().InverseTransformVector(Offset);
            if (FMath::Max(FMath::Abs(Local.X), FMath::Abs(Local.Y)) > .43 * Definition.GridCellSize) continue;
            Nearest = Distance;
            GoalPosition = Goal.GridPosition;
        }
        if (GoalPosition) return GoalPosition;
    }

    if (!bHit || Box || bLegacyCrate) return {};
    const auto Tile = PickTile(Hit);
    if (Tile && Tile->Z > 0) return Tile;
    return PickObstacle(Hit);
}

void AShengLuBoard::SelectPlayerCell()
{
    if (!bPuzzleReady || IsFirstPerson() || IsAnimating() || IsRestored() || WaterInFlight()) return;
    if (State.PlayerWindTurns > 0 && !IsPlayerLevitating()) return;
    bPawnSelected = true;
    Notice.Reset();
}

void AShengLuBoard::ClickAt(FVector2D Mouse)
{
    auto* PC=Cast<APlayerController>(Controller);
    if (!PC || (IsFirstPerson() && !IsShowingResult()) || IsAnimating()) return;
    if (auto* HUD=Cast<AShengLuHUD>(PC->GetHUD()); HUD && HUD->TryClick(Mouse)) return;
    if (!bPuzzleReady || IsRestored()) return;
    FHitResult Hit;
    if (!PC->GetHitResultAtScreenPosition(Mouse,ECC_Visibility,false,Hit)) return;
    const TOptional<FIntVector> Tile = Hit.GetComponent()==HeroPick ? TOptional<FIntVector>(State.PlayerPosition) : PickTile(Hit);
    if (Tile == State.PlayerPosition) { SelectPlayerCell(); return; }
    if (!bPawnSelected || !Tile) return;
    for (const auto Step:Steps)
        if (StepDestination(Step)==*Tile) { Apply(Step); return; }
    const FIntVector Delta = *Tile - State.PlayerPosition;
    if (Delta.Z > 0 && FMath::Abs(Delta.X) + FMath::Abs(Delta.Y) == 1 && RuneSkills::IsUnlocked(Definition, ERuneAction::Levitate))
        Notice = TEXT("先按 2 瞄准脚下并左键，升到台顶同层");
}

void AShengLuBoard::ClickBoard()
{
    if (IsFirstPerson() && !IsShowingResult())
    {
        if (IsAnimating()) return;
        if (Selected==ERuneAction::Levitate || Selected==ERuneAction::WaterAnchor)
        {
            if (CastPhase==ECastPhase::Ready && !IsRestored()) StartCast();
            Confirm();
        }
        else if (Selected==ERuneAction::Pulse && CastPhase==ECastPhase::Ready && !IsRestored()) StartCast();
        else if (Selected==ERuneAction::Hiss) Confirm();
        return;
    }
    auto* PC=Cast<APlayerController>(Controller);
    FVector2D Mouse;
    if (PC && PC->GetMousePosition(Mouse.X,Mouse.Y)) ClickAt(Mouse);
}

void AShengLuBoard::UpdateAim()
{
    const FVector Origin=Eyes->GetComponentLocation();
    const float Distance=Selected==ERuneAction::WaterAnchor?10000.f:Selected==ERuneAction::Pulse?4000.f:3000.f;
    const FVector Far=Origin+Eyes->GetForwardVector()*Distance;
    FHitResult Hit;
    const bool HitSurface=GetWorld()->LineTraceSingleByChannel(Hit,Origin,Far,ECC_Visibility);
    AimHit=Hit;
    AimPoint=HitSurface?Hit.ImpactPoint:Far;
    const int32 PreviousBoxId = TargetBoxId;
    TargetBoxId = INDEX_NONE;
    TargetCell = FPuzzleLevelDefinition::InvalidPosition;
    if (Selected == ERuneAction::Levitate && Far.Z < Origin.Z)
    {
        const double Fraction = (Origin.Z - Cell(State.PlayerPosition).Z) / (Origin.Z - Far.Z);
        if (Fraction >= 0. && Fraction <= (HitSurface ? Hit.Time + .001 : 1.))
        {
            const auto Point = FMath::Lerp(Origin, Far, Fraction);
            const auto GridPoint = LevelAsset ? Point : GetActorTransform().InverseTransformPosition(Point) + Definition.GridOrigin;
            if (Definition.WorldToGrid(GridPoint) == State.PlayerPosition)
            {
                AimPoint = Point;
                TargetCell = State.PlayerPosition;
                bAimBox = false;
                PreferredBoxId.Reset();
                return;
            }
        }
    }
    if (!UsesLegacyPresentation())
    {
        if (const auto* Pawn = Cast<ARuneBoxPawn>(Hit.GetActor()); Pawn && Pawn->GetOwner() == this)
        {
            if (const auto* Box = State.FindBox(Pawn->GetBoxId()))
            {
                TargetCell = Box->GridPosition;
                TargetBoxId = Box->BoxId;
            }
        }
        if (TargetBoxId == INDEX_NONE)
        {
            const auto Obstacle = PickObstacle(Hit);
            const auto Inside = Obstacle ? State.GetBoxesAt(*Obstacle) : TArrayView<const int32>();
            if (Obstacle && (Selected == ERuneAction::Hiss || !Inside.IsEmpty()))
            {
                TargetCell = *Obstacle;
                if (Selected != ERuneAction::Hiss) TargetBoxId = Inside[0];
            }
            else if (const auto Tile = PickTile(Hit))
            {
                TargetCell = *Tile;
                if (Selected != ERuneAction::Pulse && !State.GetBoxesAt(*Tile).IsEmpty())
                    TargetBoxId = State.GetBoxesAt(*Tile)[0];
            }
        }
        if (PreferredBoxId && (*PreferredBoxId == INDEX_NONE ? CanAimAtObstacle() : State.GetBoxesAt(TargetCell).Contains(*PreferredBoxId)))
            TargetBoxId = *PreferredBoxId;
        else PreferredBoxId.Reset();
        bAimBox = TargetBoxId != INDEX_NONE;
        if (PreviousBoxId != TargetBoxId) { HitTime = 0; bTrackingBox = false; }
        return;
    }
    bAimBox=Hit.GetComponent()==Crate || (Hit.GetComponent() && Hit.GetComponent()->IsAttachedTo(Crate));
    if (bAimBox) TargetCell=LegacyView.Box;
    else if (auto Tile=PickTile(Hit))
    {
        TargetCell=SurfaceUnder(*Tile);
        if (Selected==ERuneAction::Levitate || Selected==ERuneAction::WaterAnchor)
            bAimBox = Tile->X == LegacyView.Box.X && Tile->Y == LegacyView.Box.Y;
    }
    if (bAimBox) TargetBoxId = State.Boxes[0].BoxId;
}

bool AShengLuBoard::CanAimAtObstacle() const
{
    const auto* Cell = Definition.CellAt(TargetCell);
    return Selected == ERuneAction::Hiss && Cell && Cell->bBlocked;
}

void AShengLuBoard::CycleAimBox()
{
    if (!IsFirstPerson() || UsesLegacyPresentation()) return;
    UpdateAim();
    const auto Ids = State.GetBoxesAt(TargetCell);
    TArray<int32, TInlineAllocator<3>> Targets(Ids.GetData(), Ids.Num());
    if (CanAimAtObstacle()) Targets.Add(INDEX_NONE);
    if (Targets.Num() < 2) return;
    PreferredBoxId = Targets[(Targets.Find(TargetBoxId) + 1) % Targets.Num()];
    UpdateAim();
}

FPuzzleActionResult AShengLuBoard::Preview() const
{
    if (!Selected || (!bAimBox && !AimedAtObstacle() && !AimedAtSelf()))
        return {State, FPuzzleLevelDefinition::InvalidPosition,
            Selected == ERuneAction::Hiss ? TEXT("将准星对准符文箱或障碍物") :
            Selected == ERuneAction::Levitate ? TEXT("瞄准箱子，或向下瞄准自己脚下") : TEXT("将准星对准符文箱")};
    const FPuzzleAction Action = MakeAction(*Selected);
    if (Action.Action == ERuneAction::Pulse && Action.Direction == FIntVector::ZeroValue)
        return {State, FPuzzleLevelDefinition::InvalidPosition, TEXT("请站到施力侧，沿视线朝箱子推")};
    return Evaluate(Action);
}

void AShengLuBoard::SelectSkill(ERuneAction Skill)
{
    const bool Entering=!IsFirstPerson();
    if (!bPuzzleReady || IsShowingResult() || (Entering && (IsAnimating() || IsRestored())) || !RuneSkills::Slot(Skill)) return;
    if (!RuneSkills::IsUnlocked(Definition, Skill)) { Notice = RuneSkills::LockedMessage(Skill); return; }
    const bool ChargeOnEquip = (Skill == ERuneAction::Levitate || Skill == ERuneAction::WaterAnchor ||
        Skill == ERuneAction::Hiss) && !IsRestored();
    if (Selected==Skill && !(ChargeOnEquip && CastPhase==ECastPhase::Ready)) return;

    const bool WasWater=Selected==ERuneAction::WaterAnchor;
    bHandTransitionPlaying = false;
    ResetCast(Skill);
    const bool Water=Skill==ERuneAction::WaterAnchor;
    Eyes->PostProcessSettings.bOverride_MotionBlurAmount=!Water;
    Eyes->PostProcessSettings.bOverride_LumenFrontLayerTranslucencyReflections=
        Water || Camera->PostProcessSettings.bOverride_LumenFrontLayerTranslucencyReflections;
    Eyes->PostProcessSettings.LumenFrontLayerTranslucencyReflections=
        Water || Camera->PostProcessSettings.LumenFrontLayerTranslucencyReflections;
    if (!Entering && UsesLegacyPresentation() && (Water || WasWater))
        Eyes->SetFieldOfView(Water?90.f:Skill==ERuneAction::Levitate?100.f:88.f);
    Notice.Reset();
    if (Entering)
    {
        SetView(true);
        UpdateLook(Skill!=ERuneAction::WaterAnchor);
    }
    UpdateAim();
    if (ChargeOnEquip) StartCast(Entering);
}

void AShengLuBoard::PlayHandPhase(ECastPhase NewPhase)
{
    CastPhase = NewPhase;
    PhaseTime = 0;
    ApplyHandPose();
}

UAnimSequence* AShengLuBoard::GetCastClip(ECastPhase Phase) const
{
    const int32 Offset = FMath::Max(0, int32(Phase) - int32(ECastPhase::Start));
    if (Selected == ERuneAction::Hiss) return HissClips[Offset].Get();
    const int32 Base = Selected == ERuneAction::WaterAnchor ? 6 : Selected == ERuneAction::Levitate ? 0 : 3;
    return CastClips[Base + Offset].Get();
}

void AShengLuBoard::PlayHandTransition(UAnimSequence* Clip)
{
    if (!IsFirstPerson() || !Clip) return;
    bHandTransitionPlaying = true;
    Hands->PlayAnimation(Clip, false);
    Hands->SetPlayRate(1.f);
}

void AShengLuBoard::ApplyHandPose()
{
    if (bHandTransitionPlaying) return;
    if (!Selected) { Hands->Stop(); return; }

    const auto* Instance = Hands->GetSingleNodeInstance();
    const bool bAirborneIdle = CastPhase == ECastPhase::Ready && State.PlayerWindTurns > 0 && JumpLoopHands;
    if (bAirborneIdle)
    {
        if (!Instance || Instance->GetAnimationAsset() != JumpLoopHands || !Hands->IsPlaying())
        {
            Hands->PlayAnimation(JumpLoopHands, true);
            Hands->SetPlayRate(1.f);
        }
        return;
    }

    // The water GA changes FX at ChargeTime. The lift sequence has its own playback clock.
    if (Selected == ERuneAction::WaterAnchor && CastPhase == ECastPhase::Loop &&
        Instance && Instance->GetAnimationAsset() == GetCastClip(ECastPhase::Start) && Hands->IsPlaying()) return;
    auto* Clip = GetCastClip(CastPhase);
    Hands->PlayAnimation(Clip, CastPhase == ECastPhase::Loop);
    const bool bPulseCharge = Selected == ERuneAction::Pulse && CastPhase == ECastPhase::Start;
    Hands->SetPlayRate(bPulseCharge && Clip ? Clip->GetPlayLength() / ChargeSeconds : 1.f);
    if (CastPhase == ECastPhase::Ready)
    {
        Hands->SetPosition(0.f, false);
        Hands->Stop();
    }
}

FVector AShengLuBoard::HandAnchor() const
{
    return Charge->GetComponentLocation();
}

void AShengLuBoard::StartCast(bool bKeepHandTransition)
{
    if (!bKeepHandTransition) bHandTransitionPlaying = false;
    HitTime=0; bTrackingBox=false; bSpentHold=false;
    PlayHandPhase(ECastPhase::Start);
    if (Selected == ERuneAction::Hiss) return;
    if (Selected==ERuneAction::WaterAnchor) { StartWater(); return; }
    if (Selected==ERuneAction::Levitate)
    {
        AimAge=0.f;
        PlaySound(CastTransient,TEXT("AirCharge"));
        PlaySound(CastLoop,TEXT("AirRange"));
        SetRangeVisible(TargetCell.X>=0); return;
    }
    PlaySound(CastTransient,TEXT("ElecCharge"));
    WaveSeed=FMath::Rand();
    bReleasedCharged=false;
    if (BeamFadeUntil>=0)
    {
        Lightning->DeactivateImmediate();
        BeamArm->DeactivateImmediate();
        BeamFadeUntil=-1;
    }
    bHeld=true;
    Charge->SetVariableBool(TEXT("User.IsActive"),true);
    ChargeArm->SetVariableBool(TEXT("User.IsActive"),true);
    ChargeArm->SetVariableFloat(TEXT("User.ChargeDuration"),ChargeSeconds);
    Charge->Activate(true);
    ChargeArm->Activate(true);
}

void AShengLuBoard::SetRangeVisible(bool Visible)
{
    if (Visible)
    {
        // The Air02 ability adds 100 cm when spawning and moving the range system.
        // The range decal's own spawn program places it 100 cm below that origin.
        Range->SetWorldLocation(Cell(AimedAtSelf() ? TargetCell : SurfaceUnder(TargetCell))+FVector(0,0,100));
        UpdateWindCamera(Range);
        Range->SetVariableVec3(TEXT("User.CenterPosition"),Range->GetComponentLocation());
        Range->SetVariableBool(TEXT("User.StartCharge"),CastPhase!=ECastPhase::End);
        const float Fade=CastPhase==ECastPhase::End?1.f-FMath::Clamp(PhaseTime/.45f,0.f,1.f):1.f;
        Range->SetVariableVec4(TEXT("User.Air02Phase"),FVector4(AimAge,0.f,FMath::SmoothStep(0.f,.3f,AimAge)*Fade,Fade));
        Range->SetVariableFloat(TEXT("User.Air02FadeAge"),CastPhase==ECastPhase::End?PhaseTime:0.f);
        Range->SetVariableBool(TEXT("User.IsActive"),CastPhase!=ECastPhase::End);
        if (!Range->IsActive()) Range->Activate(true);
    }
    else if (Range->IsActive()) Range->DeactivateImmediate();
}

void AShengLuBoard::SetBeamVisual(bool Visible)
{
    const bool WasFiring=bLoopingBeam;
    bLoopingBeam=Visible;
    if (Visible)
    {
        Lightning->SetVariablePosition(TEXT("User.CenterPosition"),HandAnchor());
        Lightning->SetVariablePosition(TEXT("User.TargetPosition"),AimPoint);
        Lightning->SetVariableBool(TEXT("User.IsActive"),true);
        BeamArm->SetVariableBool(TEXT("User.IsActive"),true);
        BeamArm->SetVariableFloat(TEXT("User.ChargeDuration"),ChargeSeconds);
        Lightning->Activate(true);
        BeamArm->Activate(true);
        PlaySound(CastTransient,TEXT("ElecReady")); PlaySound(CastLoop,TEXT("ElecTrail"));
    }
    else
    {
        if (WasFiring)
        {
            // TriggerRelease closes the holding systems, then schedules their cleanup at +1 s.
            for (auto* FX : {Lightning.Get(),BeamArm.Get()})
            {
                FX->SetVariableBool(TEXT("User.IsActive"),false);
                FX->Deactivate();
            }
            BeamFadeUntil=EffectTime+1.f;
        }
        FadeContact();
        ContactGlow->SetVisibility(false);
        LastScarSample.Reset();
        bStrokeStarted=false;
        CastLoop->Stop(); ImpactLoop->Stop(); ImpactLoop->SetSound(nullptr);
        if (Ribbons->GetNumSections()>1) Ribbons->ClearMeshSection(1);
    }
}

void AShengLuBoard::EndCast()
{
    if (!Selected || CastPhase==ECastPhase::End) return;
    bReleasedCharged=IsFiring();
    bHeld=bTrackingBox=false; HitTime=0;
    Charge->SetVariableBool(TEXT("User.IsActive"),false);
    ChargeArm->SetVariableBool(TEXT("User.IsActive"),false);
    Charge->DeactivateImmediate(); ChargeArm->DeactivateImmediate();
    SetBeamVisual(false);
    if (bReleasedCharged)
    {
        BeamEnd->Activate(true);
        PlaySound(CastTransient,TEXT("ElecEnd"));
    }
    else if (Selected==ERuneAction::Levitate) PlaySound(CastTransient,TEXT("AirRelease"));
    else CastTransient->Stop();
    PlayHandPhase(ECastPhase::End);
}

void AShengLuBoard::ReleaseClick()
{
    if (Selected==ERuneAction::Pulse && bHeld) EndCast();
}

void AShengLuBoard::Confirm()
{
    if (Selected == ERuneAction::Hiss)
    {
        UpdateAim();
        const auto Result = Preview();
        if (!Result.IsValid()) { Notice = Result.Reason; return; }
        if (!IsAnimating() && !WaterInFlight()) Abilities->TryActivateAbilityByClass(UGA_Hiss::StaticClass());
        return;
    }
    if (Selected==ERuneAction::WaterAnchor && (CastPhase==ECastPhase::Start || CastPhase==ECastPhase::Loop))
    {
        UpdateAim();
        Abilities->TryActivateAbilityByClass(UGA_WaterAnchor::StaticClass());
        return;
    }
    if (Selected!=ERuneAction::Levitate || (CastPhase!=ECastPhase::Start && CastPhase!=ECastPhase::Loop) || IsAnimating()) return;
    UpdateAim();
    const auto P=Preview();
    if (!P.IsValid()) { Notice=P.Reason; return; }
    if (Abilities->TryActivateAbilityByClass(UGA_Levitate::StaticClass())) EndCast();
}

void AShengLuBoard::ResetCast(TOptional<ERuneAction> EquippedSkill)
{
    StopWaterHands();
    if (CastPhase!=ECastPhase::End) CastTransient->Stop();
    bHeld=bSpentHold=bAimBox=bPawnSelected=bTrackingBox=bReleasedCharged=false;
    HitTime=PhaseTime=0; CastPhase=ECastPhase::Ready;
    for (auto* FX : {Charge.Get(),ChargeArm.Get(),BeamEnd.Get()}) FX->DeactivateImmediate();
    SetRangeVisible(false);
    HandGlow->SetVisibility(false);
    SetBeamVisual(false);
    Selected=EquippedSkill; TargetCell=FPuzzleLevelDefinition::InvalidPosition;
    TargetBoxId=INDEX_NONE;
    PreferredBoxId.Reset();
    AimHit=FHitResult();
    ApplyHandPose();
}

void AShengLuBoard::UpdateCasting(float Dt)
{
    if (bHandTransitionPlaying && !Hands->IsPlaying())
    {
        bHandTransitionPlaying = false;
        ApplyHandPose();
    }
    if (!Selected) return;
    if (CastPhase==ECastPhase::Ready)
    {
        UpdateAim();
        return;
    }
    PhaseTime+=Dt;
    if (Selected==ERuneAction::Levitate) AimAge+=Dt;
    if (CastPhase==ECastPhase::End)
    {
        if (Selected==ERuneAction::Levitate) SetRangeVisible(PhaseTime<.45f && TargetCell.X>=0);
        const auto* Clip = GetCastClip(ECastPhase::End);
        if (!Clip || PhaseTime>=Clip->GetPlayLength())
        {
            ResetCast(Selected);
            if (Selected==ERuneAction::WaterAnchor && !IsRestored()) StartCast(true);
        }
        return;
    }
    UpdateAim();
    if (Selected==ERuneAction::WaterAnchor)
    {
        if (CastPhase==ECastPhase::Start && PhaseTime>=ChargeSeconds)
        {
            WaterCharge->DeactivateImmediate(); WaterChargeArm->DeactivateImmediate();
            PlayHandPhase(ECastPhase::Loop);
            WaterHold->SetVariableBool(TEXT("User.IsActive"),true);
            WaterHold->Activate(true); WaterHoldArm->Activate(true);
            PlaySound(CastTransient,TEXT("WaterReady")); PlaySound(CastLoop,TEXT("WaterHold"));
        }
        if (CastPhase == ECastPhase::Loop && !Hands->IsPlaying() && Hands->GetSingleNodeInstance() &&
            Hands->GetSingleNodeInstance()->GetAnimationAsset() == GetCastClip(ECastPhase::Start))
            ApplyHandPose();
        UpdateWaterHandInputs();
        WaterRange->SetWorldLocation(AimPoint);
        // Auto-charge can restart while the previous anchor is still splashing.
        WaterRange->SetVisibility(TargetCell.X>=0 && WaterAge<0.f);
        return;
    }
    if (Selected == ERuneAction::Levitate || Selected == ERuneAction::Hiss)
    {
        if (Selected == ERuneAction::Levitate) SetRangeVisible(TargetCell.X >= 0);
        if (CastPhase == ECastPhase::Start && !bHandTransitionPlaying && !Hands->IsPlaying())
            PlayHandPhase(ECastPhase::Loop);
        return;
    }
    if (!bHeld) return;
    if (CastPhase==ECastPhase::Start)
    {
        if (PhaseTime<ChargeSeconds) return;
        PlayHandPhase(ECastPhase::Loop); SetBeamVisual(true);
        return;
    }
    if (AimHit.bBlockingHit && !ImpactLoop->GetSound()) PlaySound(ImpactLoop,TEXT("ElecHit"));
    if (!AimHit.bBlockingHit && ImpactLoop->GetSound()) { ImpactLoop->Stop(); ImpactLoop->SetSound(nullptr); }
    if (!bSpentHold)
    {
        const bool OnTarget=Preview().IsValid();
        HitTime=OnTarget && bTrackingBox?FMath::Min(PulseHitSeconds,HitTime+Dt):0.f;
        bTrackingBox=OnTarget;
        if (HitTime>=PulseHitSeconds && !IsAnimating())
            bSpentHold=Abilities->TryActivateAbilityByClass(UGA_Pulse::StaticClass());
    }
}
