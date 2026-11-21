#include "ShengLuGame.h"

#include "AbilitySystemComponent.h"
#include "Algo/AllOf.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "ProceduralMeshComponent.h"
#include "RuneBoxPawn.h"
#include "SceneView.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
    constexpr int32 HissStencil = 1;
    constexpr float BoardOnlyZoom = 1.70f;
    constexpr float PlayerFramingZoom = 2.85f;
    constexpr int32 GoalMarkerSection = 0;
    constexpr int32 MoveMarkerSection = 1;

    struct FMarkerQuads
    {
        TArray<FVector> Vertices;
        TArray<int32> Indices;
        TArray<FVector2D> UVs;
        TArray<FLinearColor> Colors;

        void Add(const FTransform& MeshTransform, FVector Center, double HalfWidth, FLinearColor Style)
        {
            const int32 First = Vertices.Num();
            for (const FVector2D UV : {FVector2D(0, 0), FVector2D(1, 0), FVector2D(1, 1), FVector2D(0, 1)})
            {
                const FVector Corner((UV.X * 2. - 1.) * HalfWidth, (UV.Y * 2. - 1.) * HalfWidth, 0.);
                Vertices.Add(MeshTransform.InverseTransformPosition(Center + Corner));
                UVs.Add(UV);
                Colors.Add(Style);
            }
            Indices.Append({First, First + 1, First + 2, First, First + 2, First + 3});
        }

        void Submit(UProceduralMeshComponent& Mesh, int32 Section) const
        {
            if (!Vertices.IsEmpty())
                Mesh.CreateMeshSection_LinearColor(Section, Vertices, Indices, {}, UVs, {}, {}, {}, Colors, {}, false, false);
            else if (Mesh.GetProcMeshSection(Section))
                Mesh.ClearMeshSection(Section);
        }
    };

    void SetupBoardMesh(UStaticMeshComponent& Mesh, UStaticMesh* Cube, UMaterialInterface* Material, FLinearColor Tint)
    {
        Mesh.SetStaticMesh(Cube);
        Mesh.SetMaterial(0, Material);
        Mesh.SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        Mesh.SetCollisionResponseToAllChannels(ECR_Ignore);
        Mesh.SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
        Mesh.SetDefaultCustomPrimitiveDataVector4(0, FVector4(Tint.R, Tint.G, Tint.B, 1));
    }
}

void AShengLuBoard::SetupBoardMarkers()
{
    BoardMarkers = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PuzzleBoardMarkers"));
    BoardMarkers->SetupAttachment(RootComponent);
    BoardMarkers->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    BoardMarkers->SetCastShadow(false);
    BoardMarkers->bUseAsOccluder = false;
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> Depth(TEXT("/Game/VFX/Puzzle/M_BoardMarkers"));
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> XRay(TEXT("/Game/VFX/Puzzle/M_BoardMarkers_XRay"));
    BoardMarkerMaterial = Depth.Object;
    BoardMarkerXRayMaterial = XRay.Object;
    BoardMarkers->SetMaterial(GoalMarkerSection, BoardMarkerMaterial);
    BoardMarkers->SetMaterial(MoveMarkerSection, BoardMarkerMaterial);
}

FVector AShengLuBoard::GoalMarkerCenter(FIntVector Position) const
{
    return Cell(Position) + FVector(0, 0, Definition.GridCellSize * .0075);
}

void AShengLuBoard::RebuildGoalMarkers(bool bSolved)
{
    if (!BoardMarkerMaterial) return;
    FMarkerQuads Mesh;
    TSet<FIntVector> Cells;
    const FLinearColor Style = bSolved ? FLinearColor(.28f, .97f, .67f, 1.f) : FLinearColor(1.f, .59f, .20f, 1.f);
    const auto& Transform = BoardMarkers->GetComponentTransform();
    const double HalfWidth = Definition.GridCellSize * .5;
    for (const auto& Goal : Definition.Goals)
    {
        if (Cells.Contains(Goal.GridPosition)) continue;
        Cells.Add(Goal.GridPosition);
        Mesh.Add(Transform, GoalMarkerCenter(Goal.GridPosition), HalfWidth, Style);
    }
    Mesh.Submit(*BoardMarkers, GoalMarkerSection);
}

void AShengLuBoard::UpdateBoardMarkers()
{
    const auto* PC = Cast<APlayerController>(Controller);
    const bool bInspectKeyHeld = BoardMarkerXRayMaterial && PC && PC->IsInputKeyDown(EKeys::E);
    if (bDepthViewActive != bInspectKeyHeld)
    {
        bDepthViewActive = bInspectKeyHeld;
        auto* Material = bDepthViewActive ? BoardMarkerXRayMaterial.Get() : BoardMarkerMaterial.Get();
        BoardMarkers->SetMaterial(GoalMarkerSection, Material);
        BoardMarkers->SetMaterial(MoveMarkerSection, Material);
    }
    if (!BoardMarkerMaterial) return;

    TArray<FIntVector, TInlineAllocator<4>> Steps;
    TOptional<FIntVector> Hover, Arrival;
    if (bPawnSelected && !IsRestored())
    {
        for (ERuneAction Direction : {ERuneAction::North, ERuneAction::East, ERuneAction::South, ERuneAction::West})
            if (const auto Destination = StepDestination(Direction)) Steps.Add(*Destination);
        if (const auto Hit = HoverCell(); Hit && Steps.Contains(*Hit)) Hover = Hit;
        if (IsAnimating()) Arrival = State.PlayerPosition;
    }
    if (Steps == MarkedSteps && Hover == MarkedHover && Arrival == MarkedArrival) return;
    MarkedSteps = MoveTemp(Steps);
    MarkedHover = Hover;
    MarkedArrival = Arrival;

    FMarkerQuads Mesh;
    const auto& Transform = BoardMarkers->GetComponentTransform();
    const double HalfWidth = Definition.GridCellSize * .5;
    const FVector Lift(0, 0, Definition.GridCellSize * .015);
    const FLinearColor Mint(.37f, .96f, .77f, 0.f), Gold(1.f, .68f, .28f, .5f);
    for (const FIntVector Position : MarkedSteps)
        Mesh.Add(Transform, Cell(Position) + Lift, HalfWidth, Hover == Position ? Gold : Mint);
    if (Arrival) Mesh.Add(Transform, Cell(*Arrival) + Lift, HalfWidth, Gold);
    Mesh.Submit(*BoardMarkers, MoveMarkerSection);
}

void AShengLuBoard::BuildAssetPresentation()
{
    TInlineComponentArray<UStaticMeshComponent*> Components(this);
    for (auto* Component : Components)
    {
        if (!Component->ComponentHasTag(TEXT("LegacyGeometry"))) continue;
        Component->SetVisibility(false);
        Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    }

    const auto MakeTerrain = [&](FName Name, FLinearColor Tint)
    {
        auto* Mesh = NewObject<UInstancedStaticMeshComponent>(this, Name, RF_Transient);
        Mesh->SetupAttachment(RootComponent);
        SetupBoardMesh(*Mesh, Cube, Stone, Tint);
        Mesh->RegisterComponent();
        return Mesh;
    };
    Floors = MakeTerrain(TEXT("PuzzleFloors"), {.24f, .35f, .38f});
    Walls = MakeTerrain(TEXT("PuzzleWalls"), {.35f, .40f, .51f});
    TArray<FTransform> FloorTransforms;
    for (int32 Index = 0; Index < Definition.StaticCells.Num(); ++Index)
    {
        const auto& Terrain = Definition.StaticCells[Index];
        const FIntVector P = Definition.IndexToGrid(Index);
        if (Terrain.bHasFloor)
        {
            const FBox Bounds = Definition.FloorBounds(P);
            FloorTransforms.Emplace(FQuat::Identity, Bounds.GetCenter(), Bounds.GetSize() / 100.);
            FloorPositions.Add(P);
        }
    }
    Floors->AddInstances(FloorTransforms, false, true, false);
    UpdateObstaclePresentation();

    const float HeroScale = FMath::Min(1.f, FMath::Min(Definition.GridCellSize, Definition.LayerHeight()) / 200.f);
    Hero->SetWorldScale3D(FVector(HeroScale));
    const FVector Focus = (Cell(FIntVector::ZeroValue) + Cell(Definition.GridDimensions - FIntVector(1))) * .5;
    const double Size = FMath::Max(Definition.GridDimensions.GetMax() * double(Definition.GridCellSize), 200.);
    const FVector Offset = FVector(-1.35, -1.1, 1.4) * Size;
    Camera->SetWorldLocationAndRotation(Focus + Offset, (-Offset).Rotation());
}

bool AShengLuBoard::SpawnBoxPawns()
{
    FActorSpawnParameters Params;
    Params.Owner = this;
    Params.ObjectFlags |= RF_Transient;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    for (const auto& Box : State.Boxes)
    {
        const FVector Center = LevelAsset ? Definition.BoxBounds(Box.GridPosition, Box.bHiss).GetCenter() : BoxCenter(Box.GridPosition);
        auto* Pawn = GetWorld()->SpawnActor<ARuneBoxPawn>(Center, FRotator::ZeroRotator, Params);
        if (!Pawn)
        {
            Notice = FString::Printf(TEXT("无法生成符文箱 #%d"), Box.BoxId);
            UE_LOG(LogTemp, Error, TEXT("%s"), *Notice);
            for (const auto& Entry : BoxPawns) Entry.Value->Destroy();
            BoxPawns.Reset();
            return false;
        }
        BoxPawns.Add(Box.BoxId, Pawn);
        if (LevelAsset) SetupBoardMesh(*Pawn->GetBoxMesh(), Cube, Stone, {.095f, .46f, .44f});
        else
        {
            Pawn->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepWorldTransform);
            Crate->AttachToComponent(Pawn->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
            Crate->SetRelativeLocation(FVector::ZeroVector);
        }
    }
    return true;
}

void AShengLuBoard::UpdateObstaclePresentation()
{
    for (auto It = HissWallMeshes.CreateIterator(); It; ++It)
    {
        if (State.HissObstacles.Contains(It.Key())) continue;
        It.Value()->DestroyComponent();
        It.RemoveCurrent();
    }
    WallPositions.Reset();
    TArray<FTransform> WallTransforms;
    for (int32 Index = 0; Index < Definition.StaticCells.Num(); ++Index)
    {
        if (!Definition.StaticCells[Index].bBlocked) continue;
        const FIntVector Position = Definition.IndexToGrid(Index);
        const FBox Bounds = Definition.BlockBounds(Position);
        const FTransform Transform(FQuat::Identity, Bounds.GetCenter(), Bounds.GetSize() / 100.);
        if (!State.HissObstacles.Contains(Position))
        {
            WallTransforms.Add(Transform);
            WallPositions.Add(Position);
        }
        else if (!HissWallMeshes.Contains(Position))
        {
            auto* Mesh = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
            Mesh->SetupAttachment(RootComponent);
            SetupBoardMesh(*Mesh, Cube, HissSurface ? HissSurface.Get() : Stone.Get(), {.59f, .27f, .88f});
            Mesh->SetRelativeTransform(Transform);
            Mesh->SetCustomDepthStencilValue(HissStencil);
            Mesh->SetRenderCustomDepth(true);
            Mesh->RegisterComponent();
            HissWallMeshes.Add(Position, Mesh);
        }
    }
    Walls->ClearInstances();
    Walls->AddInstances(WallTransforms, false, true, false);
}

void AShengLuBoard::UpdateBoardPresentation(bool bSnapToGrid)
{
    const bool bSolved = IsRestored();
    if (bSnapToGrid || bSolved) RebuildGoalMarkers(bSolved);
    const bool bHasHiss = !State.HissObstacles.IsEmpty() ||
        State.Boxes.ContainsByPredicate([](const FRuneBoxState& Box) { return Box.bHiss; });
    if (bHasHiss && !HissSurface)
        HissSurface = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/HissOpticalFlow/Mesh/M_02_Inst.M_02_Inst"));
    if (Walls && (HissWallMeshes.Num() != State.HissObstacles.Num() ||
        !Algo::AllOf(HissWallMeshes, [this](const auto& Pair) { return State.HissObstacles.Contains(Pair.Key); })))
        UpdateObstaclePresentation();

    for (const auto& Box : State.Boxes)
    {
        auto* Pawn = BoxPawns.FindRef(Box.BoxId).Get();
        if (!IsValid(Pawn)) continue;
        Pawn->SyncState(Box);
        if (!LevelAsset)
        {
            if (bSnapToGrid) Pawn->SetActorLocation(BoxCenter(Box.GridPosition));
            continue;
        }
        const auto* ASC = Pawn->GetAbilitySystemComponent();
        const bool bHiss = ASC->HasMatchingGameplayTag(RuneBoxTags::Hiss);
        const bool bPhaseLocked = ASC->HasMatchingGameplayTag(RuneBoxTags::PhaseLocked);
        const FBox Bounds = Definition.BoxBounds(Box.GridPosition, bHiss);
        auto* Mesh = Pawn->GetBoxMesh();
        Mesh->SetWorldScale3D(Bounds.GetSize() / 100.);
        if (bSnapToGrid) Pawn->SetActorLocation(Bounds.GetCenter());
        const FLinearColor Tint = bHiss ? FLinearColor(.59f, .27f, .88f) :
            bPhaseLocked ? FLinearColor(.16f, .46f, .84f) : FLinearColor(.095f, .46f, .44f);
        Mesh->SetCustomPrimitiveDataVector4(0, FVector4(Tint.R, Tint.G, Tint.B, 1));
        Mesh->SetMaterial(0, bHiss && HissSurface ? HissSurface.Get() : Stone.Get());
        Mesh->SetCustomDepthStencilValue(HissStencil);
        Mesh->SetRenderCustomDepth(bHiss);
    }
    UpdateHissEffect(bHasHiss, bSnapToGrid);
}

void AShengLuBoard::UpdateHissEffect(bool bActive, bool bReset)
{
    if (!bActive)
    {
        if (HissFlow && HissPost->bEnabled)
        {
            HissFlow->DeactivateImmediate();
            HissPost->bEnabled = false;
        }
        return;
    }

    if (!HissFlow)
    {
        auto* System = LoadObject<UNiagaraSystem>(nullptr,
            TEXT("/Game/HissOpticalFlow/Hiss/NS_HissOpticalFlow.NS_HissOpticalFlow"));
        auto* Material = LoadObject<UMaterialInterface>(nullptr,
            TEXT("/Game/HissOpticalFlow/Hiss/PostMaterial/MI_HissOpticalFlow_PostForDemo2.MI_HissOpticalFlow_PostForDemo2"));
        auto* CompositeTemplate = LoadObject<UTextureRenderTarget2D>(nullptr,
            TEXT("/Game/HissOpticalFlow/Hiss/RT_HissOpticalFlow_Composite.RT_HissOpticalFlow_Composite"));
        auto* DepthTemplate = LoadObject<UTextureRenderTarget2D>(nullptr,
            TEXT("/Game/HissOpticalFlow/Hiss/RT_HissOpticalFlow_Depth.RT_HissOpticalFlow_Depth"));
        if (!System || !Material || !CompositeTemplate || !DepthTemplate) return;

        // A single screen-space simulation samples all marked meshes in this board's view.
        HissComposite = NewObject<UTextureRenderTarget2D>(this, NAME_None, RF_Transient, CompositeTemplate);
        HissDepth = NewObject<UTextureRenderTarget2D>(this, NAME_None, RF_Transient, DepthTemplate);
        HissComposite->UpdateResource();
        HissDepth->UpdateResource();
        HissFlow = NewObject<UNiagaraComponent>(this, TEXT("HissOpticalFlow"), RF_Transient);
        HissFlow->SetupAttachment(RootComponent);
        HissFlow->SetAutoActivate(false);
        HissFlow->SetAsset(System);
        HissFlow->SetVariableFloat(TEXT("User.HissTargetStencil"), HissStencil);
        HissFlow->SetVariableTextureRenderTarget(TEXT("User.OutputRT"), HissComposite);
        HissFlow->SetVariableTextureRenderTarget(TEXT("User.DepthRT"), HissDepth);
        HissFlow->RegisterComponent();

        auto* Instance = UMaterialInstanceDynamic::Create(Material, this);
        Instance->SetTextureParameterValue(TEXT("HissBuffer"), HissComposite);
        Instance->SetTextureParameterValue(TEXT("HissDepthBuffer"), HissDepth);
        HissPost = NewObject<UPostProcessComponent>(this, TEXT("HissPostProcess"), RF_Transient);
        HissPost->SetupAttachment(RootComponent);
        HissPost->bUnbound = true;
        HissPost->bEnabled = false;
        HissPost->Settings.AddBlendable(Instance, 1.f);
        HissPost->RegisterComponent();
    }

    if (bReset || !HissPost->bEnabled)
    {
        HissComposite->UpdateResourceImmediate();
        HissDepth->UpdateResourceImmediate();
        HissFlow->Activate(true);
        HissPost->bEnabled = true;
    }
}

void AShengLuBoard::ZoomOverview(float Wheel)
{
    if (!bPuzzleReady || IsFirstPerson() || IsShowingResult() || FMath::IsNearlyZero(Wheel)) return;
    TargetOverviewZoom = FMath::Clamp(TargetOverviewZoom * FMath::Pow(1.17f, Wheel), 1.f, OverviewZoomLimit);
}

void AShengLuBoard::UpdateBoardCamera(float DeltaSeconds)
{
    auto* PC = Cast<APlayerController>(Controller);
    if (!PC) return;
    int32 Width, Height;
    PC->GetViewportSize(Width, Height);
    if (Width <= 0 || Height <= 0) return;
    const float Aspect = float(Width) / Height;
    const auto& D = Definition.GridDimensions;
    const float Diagonal = (D.X + D.Y) * Definition.GridCellSize;
    const float DefaultWidth = LevelAsset
        ? FMath::Max(Diagonal, (Diagonal * .46f + D.Z * Definition.LayerHeight() * .78f) * Aspect)
        : ShengLuLayout::Scale * FMath::Max(920.f, 740.f * Aspect);
    // The overview camera maintains its vertical span. Keep the whole character in the close view.
    const float CloseHeight = FMath::Max(4.f * HeroPick->GetScaledCapsuleHalfHeight(),
        3.3f * HeroPick->GetScaledCapsuleRadius() / Aspect);
    OverviewZoomLimit = FMath::Max(1.f, DefaultWidth / FMath::Max(1.f, CloseHeight));
    TargetOverviewZoom = FMath::Min(TargetOverviewZoom, OverviewZoomLimit);
    OverviewZoom = FMath::Min(OverviewZoomLimit, FMath::FInterpTo(OverviewZoom, TargetOverviewZoom, DeltaSeconds, 15.f));
    Camera->OrthoWidth = DefaultWidth / OverviewZoom;
}

void AShengLuBoard::CalcCamera(float DeltaSeconds, FMinimalViewInfo& OutView)
{
    Super::CalcCamera(DeltaSeconds, OutView);
    if (OutView.ProjectionMode != ECameraProjectionMode::Orthographic) return;

    OutView.OffCenterProjectionOffset.Y += .56 * (FMath::Min(OverviewZoom, BoardOnlyZoom) - 1.f);
    const float FocusEnd = FMath::Min(OverviewZoomLimit, PlayerFramingZoom);
    const float FocusStart = FMath::Min(BoardOnlyZoom, FMath::Lerp(1.f, FocusEnd, .7f));
    if (OverviewZoom <= FocusStart) return;

    const auto* PC = Cast<APlayerController>(Controller);
    if (!PC) return;
    int32 Width, Height;
    PC->GetViewportSize(Width, Height);
    if (Width <= 0 || Height <= 0) return;

    // Use the same orthographic near-plane correction as the player view before centering the model.
    FMinimalViewInfo View = OutView;
    View.SetCameraToViewTarget(GetActorLocation());
    FSceneViewProjectionData Projection;
    Projection.SetViewRectangle(FIntRect(0, 0, Width, Height));
    Projection.ViewOrigin = View.Location;
    Projection.ViewRotationMatrix = FInverseRotationMatrix(View.Rotation) *
        FMatrix(FPlane(0, 0, 1, 0), FPlane(1, 0, 0, 0), FPlane(0, 1, 0, 0), FPlane(0, 0, 0, 1));
    FMinimalViewInfo::CalculateProjectionMatrixGivenViewRectangle(View, AspectRatio_MaintainYFOV, Projection.GetViewRect(), Projection);
    const FVector4 PlayerClip = Projection.ComputeViewProjectionMatrix().TransformPosition(HeroPick->GetComponentLocation());
    const float Follow = FMath::SmoothStep(FocusStart, FocusEnd, OverviewZoom);
    OutView.OffCenterProjectionOffset += FVector2D(PlayerClip.X, PlayerClip.Y) * Follow;
}

FVector AShengLuBoard::WorldFacing() const
{
    return (Cell({State.Facing.X, State.Facing.Y, 0}) - Cell(FIntVector::ZeroValue)).GetSafeNormal();
}

FIntVector AShengLuBoard::SurfaceUnder(FIntVector Position) const
{
    if (UsesLegacyPresentation()) return {Position.X, Position.Y, Position.X == 2 && Position.Y == 1 ? 1 : 0};
    while (Position.Z > 0 && !FPuzzleSolver::HasStaticSupport(Definition, Position)) --Position.Z;
    return Position;
}

UStaticMeshComponent* AShengLuBoard::BoxMesh(int32 BoxId) const
{
    const auto* Pawn = BoxPawns.FindRef(BoxId).Get();
    return IsValid(Pawn) ? (LevelAsset ? Pawn->GetBoxMesh() : Crate.Get()) : nullptr;
}

const UMeshComponent* AShengLuBoard::CrateMesh() const
{
    return State.Boxes.IsEmpty() ? Crate.Get() : BoxMesh(State.Boxes[0].BoxId);
}

TOptional<FIntVector> AShengLuBoard::PickTile(const FHitResult& Hit) const
{
    const auto* Component = Hit.GetComponent();
    if (!Component) return {};
    if (!LevelAsset)
    {
        if (!Component->ComponentHasTag(TEXT("BoardSurface"))) return {};
        const FVector Local = GetActorTransform().InverseTransformPosition(Hit.ImpactPoint);
        FIntVector Position = Definition.WorldToGrid(Local + Definition.GridOrigin);
        Position.Z = 0;
        return Position;
    }
    if (Component == Floors && FloorPositions.IsValidIndex(Hit.Item)) return FloorPositions[Hit.Item];
    if (const auto Obstacle = PickObstacle(Hit))
    {
        const FIntVector Top = *Obstacle + FIntVector(0, 0, 1);
        if (Hit.ImpactNormal.Z > .5 && Definition.IsValidGridCoordinate(Top)) return Top;
    }
    return {};
}

TOptional<FIntVector> AShengLuBoard::PickObstacle(const FHitResult& Hit) const
{
    const auto* Component = Hit.GetComponent();
    if (Component == Walls && WallPositions.IsValidIndex(Hit.Item)) return WallPositions[Hit.Item];
    for (const auto& Pair : HissWallMeshes)
        if (Component == Pair.Value) return Pair.Key;
    return {};
}
