#if WITH_DEV_AUTOMATION_TESTS
#include "ShengLuGame.h"
#include "Camera/CameraActor.h"
#include "Components/StaticMeshComponent.h"
#include "Camera/CameraComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "InputKeyEventArgs.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraSimCache.h"
#include "NiagaraSystem.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "UnrealClient.h"

namespace
{
    class FWindSequence : public IAutomationLatentCommand
    {
        FAutomationTestBase& Test;
        TWeakObjectPtr<AShengLuBoard> Game;
        TWeakObjectPtr<ACameraActor> Observer;
        TWeakObjectPtr<AStaticMeshActor> ComparisonFloor;
        TArray<FString> Captures;
        TArray<FString> Counts={TEXT("phase,system,emitter,particles")};
        TMap<FString,FString> NamedOpeningFrames;
        double Started=-1.;
        int32 Step=0, MovieFrame=0;
        float NextMovieFrame=0.f;
        bool WasHUDVisible=true;
        TWeakObjectPtr<AHUD> HiddenHUD;
        void HideGuides(AHUD* HUD)
        {
            if (!HUD || HiddenHUD.IsValid()) return;
            WasHUDVisible=HUD->bShowHUD;
            HiddenHUD=HUD;
            HUD->bShowHUD=false;
        }
        bool RequestedOverview=false;
        bool FirstPersonCaptured=false;
        bool CaughtAimLight=false;
        bool CaughtRingBirth=false;
        bool CaughtFlashPeak=false;
        TWeakObjectPtr<UStaticMeshComponent> HiddenReceiver;
        TWeakObjectPtr<UCameraComponent> ShapeCamera;
        FRotator ShapeCameraRotation;
        int32 ShapeCaptureStep=0;

        void RecordWindSheets(UNiagaraComponent* Wind, const FVector& Eye)
        {
            auto* Cache=NewObject<UNiagaraSimCache>();
            if (!Test.TestTrue(TEXT("Read both sustained wind sheets"),
                Cache->BeginWrite(FNiagaraSimCacheCreateParameters::CreateForDebugging(),Wind) && Cache->WriteFrame(Wind) && Cache->EndWrite())) return;
            int32 SheetCount=0;
            for (const auto& Handle : Wind->GetAsset()->GetEmitterHandles())
            {
                const FName Name=Handle.GetName();
                if (Name!=TEXT("Minimal003") && Name!=TEXT("Minimal012")) continue;
                ++SheetCount;
                const auto* Emitter=Handle.GetEmitterData();
                if (!Test.TestNotNull(TEXT("Wind sheet emitter data"),Emitter)) continue;
                const auto& Renderers=Emitter->GetRenderers();
                const auto* Mesh=Renderers.Num()==1?Cast<UNiagaraMeshRendererProperties>(Renderers[0]):nullptr;
                if (!Test.TestNotNull(TEXT("Each wind sheet uses its source mesh renderer"),Mesh)) continue;
                TArray<FVector> Positions,Scales;
                TArray<FQuat> Orientations;
                TArray<FLinearColor> Colors;
                Cache->ReadPositionAttribute(Positions,TEXT("Position"),Name);
                Cache->ReadVectorAttribute(Scales,TEXT("Scale"),Name);
                Cache->ReadQuatAttribute(Orientations,TEXT("MeshOrientation"),Name);
                Cache->ReadColorAttribute(Colors,TEXT("Color"),Name);
                if (!Test.TestTrue(Name.ToString()+TEXT(" has one live mesh with pose, size and tint"),
                    Positions.Num()==1 && Orientations.Num()==1 && Scales.Num()==1 && Colors.Num()==1)) continue;
                const FVector TowardsEye=(Eye-Positions[0]).GetSafeNormal2D();
                const FVector ParticleFacing=Orientations[0].RotateVector(FVector::ForwardVector).GetSafeNormal2D();
                const double Alignment=FVector::DotProduct(TowardsEye,ParticleFacing);
                UE_LOG(LogTemp,Display,TEXT("AIR02_SHEET %s origin=(%s) scale=(%s) eye=(%s) yaw=%.1f alpha=%.4f facing=%d locked=%d alignment=%.4f"),
                    *Name.ToString(),*Positions[0].ToCompactString(),*Scales[0].ToCompactString(),*Eye.ToCompactString(),
                    Orientations[0].Rotator().Yaw,Colors[0].A,int32(Mesh->FacingMode),int32(Mesh->bLockedAxisEnable),Alignment);
                Test.TestTrue(Name.ToString()+TEXT(" uses only the source particle yaw"),
                    Mesh->FacingMode==ENiagaraMeshFacingMode::Default && !Mesh->bLockedAxisEnable && Alignment>.998);
                const FVector Expected=Name==TEXT("Minimal003")?FVector(11,7,10):FVector(15,10,6);
                Test.TestTrue(Name.ToString()+TEXT(" keeps its source mesh scale and visible alpha"),
                    Scales[0].Equals(Expected,.01) && Colors[0].A>.05f);
            }
            Test.TestEqual(TEXT("Check both source wind sheets"),SheetCount,2);
        }
    public:
        FWindSequence(FAutomationTestBase& T,AShengLuBoard* G) : Test(T),Game(G) {}
        ~FWindSequence()
        {
            if (auto* Cam=ShapeCamera.Get()) Cam->SetWorldRotation(ShapeCameraRotation);
            if (auto* HUD=HiddenHUD.Get()) HUD->bShowHUD=WasHUDVisible;
            if (auto* Mesh=HiddenReceiver.Get()) Mesh->SetVisibility(true,true);
            if (auto* Floor=ComparisonFloor.Get()) Floor->Destroy();
        }
        bool Update() override
        {
            auto* G=Game.Get();
            if (!G) { Test.AddError(TEXT("Lost the board during Air02")); return true; }
            auto* World=G->GetWorld();
            auto* PC=CastChecked<APlayerController>(G->GetController());
            const float Now=World->GetTimeSeconds();
            auto* Wind=CastChecked<UNiagaraComponent>(G->GetDefaultSubobjectByName(TEXT("NatureTornado")));
            auto* Range=CastChecked<UNiagaraComponent>(G->GetDefaultSubobjectByName(TEXT("WindRange")));
            bool Valid=false;
            const FVector4 Cue=Wind->GetVariableVec4(TEXT("User.Air02Phase"),Valid);
            auto Tap=[&](FKey K)
            {
                PC->InputKey(FInputKeyEventArgs::CreateSimulated(K,IE_Pressed,1));
                PC->InputKey(FInputKeyEventArgs::CreateSimulated(K,IE_Released,0));
            };
            FString Dir=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Screenshots/WindSequence"));
            FString CaptureName;
            if (FParse::Value(FCommandLine::Get(),TEXT("RuneWindCapture="),CaptureName) && !CaptureName.IsEmpty() &&
                !CaptureName.Contains(TEXT("..")) && CaptureName==FPaths::GetCleanFilename(CaptureName))
                Dir/=CaptureName;
            const bool Record=FParse::Param(FCommandLine::Get(),TEXT("RuneWindMovie"));
            if (Record && Started>=0 && Now>=NextMovieFrame)
            {
                FScreenshotRequest::RequestScreenshot(Dir/TEXT("Motion")/FString::Printf(TEXT("Wind%04d.png"),MovieFrame++),false,false);
                NextMovieFrame+=1.f/15.f;
            }
            const bool CaptureOpening=FParse::Param(FCommandLine::Get(),TEXT("RuneWindOpening"));
            FString OpeningFrame;
            if (CaptureOpening && Step>=4 && Step<=6 && Wind->IsActive() && Cue.X>=0. && Cue.X<=.251)
            {
                const FString File=FString::Printf(TEXT("T%04d.png"),FMath::RoundToInt(Cue.X*1000.));
                OpeningFrame=Dir/TEXT("Opening")/File;
                FScreenshotRequest::RequestScreenshot(OpeningFrame,false,false);
            }
            auto Shot=[&](const TCHAR* Name)
            {
                const auto& Board=G->Board();
                Captures.Add(FString::Printf(TEXT("%s,%.4f,%.4f,%.4f,%.4f,%d,%d"),Name,Wind->IsActive()?Cue.X:-1.,Cue.Y,Cue.Z,Cue.W,Board.SP,Board.Wind));
                for (UNiagaraComponent* FX : {Range,Wind})
                {
                    if (!FX->IsActive()) continue;
                    auto* Cache=NewObject<UNiagaraSimCache>();
                    if (!Cache->BeginWrite(FNiagaraSimCacheCreateParameters::CreateForDebugging(),FX) || !Cache->WriteFrame(FX) || !Cache->EndWrite()) continue;
                    for (int32 I=0;I<Cache->GetNumEmitters();++I)
                        Counts.Add(FString::Printf(TEXT("%s,%s,%s,%d"),Name,FX==Wind?TEXT("Tornado"):TEXT("Range"),
                            *Cache->GetEmitterName(I).ToString(),Cache->GetEmitterNumInstances(I,0)));
                }
                if (FParse::Param(FCommandLine::Get(),TEXT("RuneScreens")))
                {
                    const FString Filename=Dir/FString(Name)+TEXT(".png");
                    if (!OpeningFrame.IsEmpty()) NamedOpeningFrames.Add(Filename,OpeningFrame);
                    FScreenshotRequest::RequestScreenshot(Filename,false,false);
                }
            };
            auto CheckOrbit=[&](UNiagaraComponent* Component,FName Emitter)
            {
                auto* Cache=NewObject<UNiagaraSimCache>();
                if (!Test.TestTrue(TEXT("Read the live Air02 particle buffer"),Cache->BeginWrite(FNiagaraSimCacheCreateParameters::CreateForDebugging(),Component) && Cache->WriteFrame(Component) && Cache->EndWrite())) return;
                if (Component==Wind && Emitter==TEXT("Runes"))
                {
                    TArray<FVector> Stones;
                    TArray<int32> MeshIndices;
                    Cache->ReadPositionAttribute(Stones,TEXT("Position"),TEXT("Stone"));
                    Cache->ReadIntAttribute(MeshIndices,TEXT("MeshIndex"),TEXT("Stone"));
                    TSet<int32> Meshes;
                    for (int32 I : MeshIndices) Meshes.Add(I);
                    Test.TestTrue(TEXT("Original stone meshes orbit inside the sustained wind"),Stones.Num()>0 && Meshes.Num()==3);
                    TArray<float> Lives;
                    Cache->ReadFloatAttribute(Lives,TEXT("Lifetime"),TEXT("Stone"));
                    Test.TestTrue(TEXT("Stone lifetimes use the original GPU distribution"),Lives.Num()==Stones.Num() && !Lives.ContainsByPredicate([](float Life) { return Life<1.2999f || Life>1.8001f; }));
                    const FVector Foot=Wind->GetComponentLocation();
                    double MaxRadius=0.,MaxHeight=0.;
                    for (const FVector& Stone : Stones)
                    {
                        MaxRadius=FMath::Max(MaxRadius,(Stone-Foot).Size2D());
                        MaxHeight=FMath::Max(MaxHeight,Stone.Z-Foot.Z);
                    }
                    UE_LOG(LogTemp,Display,TEXT("AIR02_NATIVE_STONES radius=%.1f height=%.1f"),MaxRadius,MaxHeight);
                    Test.TestTrue(TEXT("The stone orbit uses the original world dimensions"),MaxRadius>200. && MaxHeight>200.);
                    Test.TestTrue(TEXT("All stones follow the source radius and height curves"),!Stones.ContainsByPredicate([Foot](FVector Point) {
                        const FVector Delta=Point-Foot;
                        return Delta.Size2D()>559.1 || Delta.Z<-.5 || Delta.Z>750.1;
                    }));
                    for (FName Layer : {FName(TEXT("LineSource")),FName(TEXT("Minimal011"))})
                    {
                        TArray<FVector> Positions;
                        Cache->ReadPositionAttribute(Positions,TEXT("Position"),Layer);
                        UE_LOG(LogTemp,Display,TEXT("AIR02_SIM %s particles=%d"),*Layer.ToString(),Positions.Num());
                    }
                }
                TArray<FVector> Positions;
                Cache->ReadPositionAttribute(Positions,TEXT("Position"),Emitter);
                Test.TestEqual(Emitter.ToString()+TEXT(" individual glyphs"),Positions.Num(),6);
                if (Positions.Num()==6)
                {
                    TArray<FVector4> Style,Origins;
                    Cache->ReadVector4Attribute(Style,TEXT("DynamicMaterialParameter"),Emitter);
                    Cache->ReadVector4Attribute(Origins,TEXT("DynamicMaterialParameter1"),Emitter);
                    TArray<FLinearColor> Tint;
                    Cache->ReadColorAttribute(Tint,TEXT("Color"),Emitter);
                    if (Style.Num()==6 && Origins.Num()==6 && Tint.Num()==6)
                        UE_LOG(LogTemp,Display,TEXT("AIR02_GLYPH_LIVE %s %s atlasPhase=%.5f blink=%.5f blur=%.3f anchor=(%.1f,%.1f,%.1f) hdr=(%.2f,%.2f,%.2f) alpha=%.4f"),*Component->GetName(),*Emitter.ToString(),Style[0].X,Style[0].Y,Style[0].Z,Origins[0].X,Origins[0].Y,Origins[0].Z,Tint[0].R,Tint[0].G,Tint[0].B,Tint[0].A);
                    Test.TestTrue(TEXT("Every glyph has the source phase, blur strength, camera anchor and RGBA"),Style.Num()==6 && Origins.Num()==6 && Tint.Num()==6 &&
                        !Style.ContainsByPredicate([Component,Range](const FVector4& V){return !FMath::IsNearlyEqual(V.Z,Component==Range?1.:.5,.001);}));
                    TArray<int32> Glyphs;
                    for (const FVector4& Parameters : Style)
                    {
                        const double Phase=Parameters.X;
                        Glyphs.Add(FMath::FloorToInt(8.*(Phase-FMath::FloorToDouble(Phase))));
                    }
                    Test.TestTrue(TEXT("Six dynamic phase values address the source eight-cell rune texture"),Glyphs.Num()==6 &&
                        !Style.ContainsByPredicate([](const FVector4& V){return !FMath::IsFinite(V.X) || V.X<0. || V.X>1.;}) &&
                        !Glyphs.ContainsByPredicate([](int32 Index){return Index<0 || Index>7;}));
                    const FString Cells=FString::JoinBy(Glyphs,TEXT(","),[](int32 Index){return FString::FromInt(Index);});
                    UE_LOG(LogTemp,Display,TEXT("AIR02_GLYPH_ATLAS %s %s cells=%s"),*Component->GetName(),*Emitter.ToString(),*Cells);
                    const float Diameter=FVector::Dist2D(Positions[0],Positions[3]);
                    // Source radii in cm, measured after their opening expansion.
                    const float SourceDiameter=Component==Range?280.f:Emitter==TEXT("Runes001")?600.f:400.f;
                    UE_LOG(LogTemp,Display,TEXT("AIR02_NATIVE_ORBIT %s %s diameter=%.2f native=%.0f"),*Component->GetName(),*Emitter.ToString(),Diameter,SourceDiameter);
                    Test.TestTrue(TEXT("Glyph orbit uses the original diameter in centimeters"),FMath::IsNearlyEqual(Diameter,SourceDiameter,1.f));
                }
            };
            if (Started>=0 && Now-Started>22.)
            {
                Test.AddError(FString::Printf(TEXT("Air02 sequence timeout step %d, age %.3f active %d"),Step,Cue.X,Wind->IsActive()));
                PC->SetViewTarget(G);
                if (Observer.IsValid() && PC->GetHUD()) PC->GetHUD()->bShowHUD=WasHUDVisible;
                if (Observer.IsValid()) Observer->Destroy();
                G->ResetPuzzle();return true;
            }
            auto SetObserver=[&](FVector Eye,FVector LookAt,float FOV)
            {
                auto* Cam=Observer.Get();
                if (!Cam)
                {
                    Cam=World->SpawnActor<ACameraActor>();
                    if (!Cam) {Test.AddError(TEXT("Wind observer camera creation failed"));return;}
                    auto* Eyes=CastChecked<UCameraComponent>(G->GetDefaultSubobjectByName(TEXT("CastCamera")));
                    Cam->GetCameraComponent()->PostProcessSettings=Eyes->PostProcessSettings;
                    Cam->GetCameraComponent()->bConstrainAspectRatio=false;
                    Observer=Cam;
                }
                Cam->SetActorLocationAndRotation(Eye,(LookAt-Eye).Rotation());
                Cam->GetCameraComponent()->FieldOfView=FOV;
                PC->SetViewTarget(Cam);
                HideGuides(PC->GetHUD());
            };
            switch (Step)
            {
            case 0:
                G->ResetPuzzle();
                {
                    auto Mesh=[&](FName Name) { return CastChecked<UStaticMeshComponent>(G->GetDefaultSubobjectByName(Name)); };
                    const FVector Size=2.*Mesh(TEXT("Tile_0_0"))->Bounds.BoxExtent;
                    const double PitchX=FVector::Dist(Mesh(TEXT("Tile_1_0"))->GetComponentLocation(),Mesh(TEXT("Tile_0_0"))->GetComponentLocation());
                    const double PitchY=FVector::Dist(Mesh(TEXT("Tile_0_1"))->GetComponentLocation(),Mesh(TEXT("Tile_0_0"))->GetComponentLocation());
                    const auto Cap=Mesh(TEXT("PlinthCap"))->Bounds.GetBox();
                    const double Top=Cap.Max.Z-G->Cell({0,0,0}).Z;
                    Test.TestTrue(TEXT("Saved board grid has 600 cm spacing on both axes"),FMath::IsNearlyEqual(PitchX,600.,.1) && FMath::IsNearlyEqual(PitchY,600.,.1));
                    Test.TestTrue(TEXT("Tiles and raised platform match the threefold geometry"),Size.Equals(FVector(582,582,66),.1) && FMath::IsNearlyEqual(Top,420.,.1));
                    UStaticMeshComponent* BoxMesh=G->Runtime().Boxes.IsEmpty()?nullptr:G->BoxMesh(G->Runtime().Boxes[0].BoxId);
                    if (!Test.TestNotNull(TEXT("Find the active crate on the saved board"),BoxMesh)) return true;
                    const FBox Body=BoxMesh->Bounds.GetBox();
                    const FVector Floor=G->Cell(G->Board().Box);
                    const FBox Inlay=Mesh(TEXT("RuneInlay"))->Bounds.GetBox();
                    Test.TestTrue(TEXT("Saved crate bounds match the puzzle definition and ground level"),
                        Body.GetSize().Equals(FVector(G->Level().BoxSide()),.1) && FMath::IsNearlyEqual(Body.Min.Z,Floor.Z,.1));
                    FHitResult Face;
                    const FVector Trace=Body.GetCenter()+FVector(0,-220,0);
                    Test.TestTrue(TEXT("The picking trace hits the active crate collision face"),World->LineTraceSingleByChannel(Face,Trace,Body.GetCenter(),ECC_Visibility) &&
                        Face.GetComponent()==BoxMesh && FMath::IsNearlyEqual(Face.ImpactPoint.Y,Body.Min.Y,.1));
                    UE_LOG(LogTemp,Display,TEXT("AIR02_CRATE side_cm=%.1f inlay_cm=%s base_z=%.1f floor_z=%.1f"),Body.GetSize().X,*Inlay.GetSize().ToString(),Body.Min.Z,Floor.Z);
                    UE_LOG(LogTemp,Display,TEXT("AIR02_BOARD pitch_cm=%.1f,%.1f tile_cm=%s platform_cm=%.1f"),PitchX,PitchY,*Size.ToString(),Top);
                }
                if (FParse::Param(FCommandLine::Get(),TEXT("RuneWindIsolateReceiver")))
                {
                    HideGuides(PC->GetHUD());
                    if (auto* Receiver=G->BoxMesh(G->Runtime().Boxes[0].BoxId); Receiver && Receiver->IsVisible())
                    { HiddenReceiver=Receiver; Receiver->SetVisibility(false,true); }
                }
                Tap(EKeys::Two);Started=NextMovieFrame=Now;
                if (Record) IFileManager::Get().MakeDirectory(*(Dir/TEXT("Motion")),true);
                if (CaptureOpening) IFileManager::Get().MakeDirectory(*(Dir/TEXT("Opening")),true);
                break;
            case 1:
                if (HiddenReceiver.IsValid() && !Observer.IsValid() && Range->IsActive())
                {
                    // A temporary receiver lets the original 10 m decal and 18 m wave extend at the board edge.
                    const FVector Foot=Range->GetComponentLocation();
                    auto* Floor=World->SpawnActor<AStaticMeshActor>();
                    if (!Floor) { Test.AddError(TEXT("Comparison floor could not be created")); return true; }
                    ComparisonFloor=Floor;
                    auto* Ground=Floor->GetStaticMeshComponent();
                    Ground->SetMobility(EComponentMobility::Movable);
                    Ground->SetCollisionEnabled(ECollisionEnabled::NoCollision);
                    Ground->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Plane.Plane")));
                    Ground->SetMaterial(0,LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial")));
                    Ground->SetWorldScale3D(FVector(60.,60.,1.));
                    Floor->SetActorLocation(Foot-FVector(0,0,1.));
                    SetObserver(Foot+FVector(-1100.,-210.,350.),Foot+FVector(0,0,140.),78.f);
                }
                if (!CaughtAimLight && Now-Started>=.09 && Now-Started<.4)
                {
                    Shot(TEXT("00-AimLight"));
                    CaughtAimLight=true;
                    return false;
                }
                if (Now-Started<.44) return false;
                Test.TestTrue(TEXT("Warm aim previsualization belongs to Range"),Range->IsActive() && !Wind->IsActive() && G->Preview().IsValid());
                Test.TestTrue(TEXT("Air02 Range origin follows the source's 100 cm aim offset"),
                    FMath::IsNearlyEqual(Range->GetComponentLocation().Z,G->Cell(G->Board().Box).Z+100.,.1));
                Shot(TEXT("01-AimStart"));break;
            case 2:
                if (Now-Started<2.55) return false;
                Test.TestTrue(TEXT("Holding aim indefinitely costs zero turns"),G->Board().SP==8 && G->Board().Turn==0 && !Wind->IsActive());
                CheckOrbit(Range,TEXT("Runes"));
                Shot(TEXT("02-AimHold"));break;
            case 3:
                Tap(EKeys::LeftMouseButton);break;
            case 4: case 5: case 6: case 7: case 8:
            {
                static constexpr float Ages[]={.07f,.225f,.413f,.83f,1.95f};
                static const TCHAR* Names[]={TEXT("03-Ring"),TEXT("04-Rim"),TEXT("05-Flash"),TEXT("06-Rising"),TEXT("07-BoardLoop")};
                const int32 Id=Step-4;
                if (!Wind->IsActive()) return false;
                if (Step==4 && !CaughtRingBirth && Cue.X>=1.f/60.f)
                {
                    Shot(TEXT("02a-RingBirth")); CaughtRingBirth=true; return false;
                }
                if (Step==5 && !CaughtFlashPeak && Cue.X>=.11f)
                {
                    Shot(TEXT("03a-FlashPeak")); CaughtFlashPeak=true; return false;
                }
                if (Cue.X<Ages[Id]) return false;
                if (Step==4)
                {
                    auto* Cache=NewObject<UNiagaraSimCache>();
                    if (Cache->BeginWrite(FNiagaraSimCacheCreateParameters::CreateForDebugging(),Wind) && Cache->WriteFrame(Wind) && Cache->EndWrite())
                    {
                        const FName Streaks=TEXT("NE_BasicEmitter_Spark001");
                        TArray<FVector> Velocities;
                        TArray<FVector2D> StreakSizes;
                        Cache->ReadVectorAttribute(Velocities,TEXT("Velocity"),Streaks);
                        Cache->ReadVector2Attribute(StreakSizes,TEXT("SpriteSize"),Streaks);
                        int32 Vertical=0;
                        for (const FVector& V : Velocities) if (V.Z>V.Size2D()) ++Vertical;
                        Test.TestEqual(TEXT("60 streaks emit upward from the rim"),Vertical,60);
                        Test.TestEqual(TEXT("Every streak has a velocity-aligned sprite"),StreakSizes.Num(),60);
                        UE_LOG(LogTemp,Display,TEXT("AIR02_RIM upward=%d live=%d at=%.4f"),Vertical,Velocities.Num(),Cue.X);
                        TArray<FVector2D> Size;
                        Cache->ReadVector2Attribute(Size,TEXT("SpriteSize"),TEXT("NE_BasicEmitter_Shockwave001"));
                        const float Width=Size.Num()?Size[0].X:0.f;
                        UE_LOG(LogTemp,Display,TEXT("AIR02_NATIVE_WAVE diameter_cm=%.1f"),Width);
                        Test.TestTrue(TEXT("Opening wave is 18 meters wide"),FMath::IsNearlyEqual(Width,1800.f,1.f));
                    }
                    else Test.AddError(TEXT("Opening wave particle buffer is unavailable"));
                }
                if (Step==4) Test.TestTrue(TEXT("Confirm spends 3 SP once, before the cyclone develops"),G->Board().SP==5 && G->Board().Wind==2 && Cue.Y==0);
                if (Step==8 && !RequestedOverview)
                {
                    Test.TestTrue(TEXT("Watching the tornado never leaves first person"),G->IsFirstPerson() && G->Phase()==ECastPhase::Ready);
                    if (!FirstPersonCaptured)
                    {
                        Shot(TEXT("07a-FirstPersonLoop"));
                        FirstPersonCaptured=true;
                        return false;
                    }
                    if (FParse::Param(FCommandLine::Get(),TEXT("RuneWindInspectShape")) && ShapeCaptureStep<2)
                    {
                        auto* Eyes=CastChecked<UCameraComponent>(G->GetDefaultSubobjectByName(TEXT("CastCamera")));
                        if (ShapeCaptureStep==0)
                        {
                            const auto* Box=G->BoxMesh(G->Runtime().Boxes[0].BoxId);
                            if (!Test.TestNotNull(TEXT("Keep the raised crate in the first-person wind comparison"),Box)) return true;
                            ShapeCamera=Eyes;
                            ShapeCameraRotation=Eyes->GetComponentRotation();
                            Eyes->SetWorldRotation((Box->Bounds.Origin-Eyes->GetComponentLocation()).Rotation());
                            ++ShapeCaptureStep;
                            return false;
                        }
                        if (Cue.X<2.15f) return false;
                        RecordWindSheets(Wind,Eyes->GetComponentLocation());
                        Shot(TEXT("07b-LiftedFirstPerson"));
                        ++ShapeCaptureStep;
                        return false;
                    }
                    if (auto* Eyes=ShapeCamera.Get()) Eyes->SetWorldRotation(ShapeCameraRotation);
                    ShapeCamera.Reset();
                    Tap(EKeys::F); RequestedOverview=true;
                    return false;
                }
                if (Step==8) Test.TestTrue(TEXT("F reveals the sustained plume from the overview"),Cue.Y>.98 && Cue.W>.98 && !G->IsFirstPerson());
                Shot(Names[Id]);break;
            }
            case 9:
            {
                // A fixed spectator in the same game world exposes the upper orbit and the late residue.
                // The previous image uses the player's normal overview camera.
                const FVector Foot=Wind->GetComponentLocation();
                const FVector Eye=Foot+FVector(-1700,360,270);
                SetObserver(Eye,Foot+FVector(0,0,450),74.f);
                break;
            }
            case 10: case 11: case 12: case 13:
            {
                static constexpr float Ages[]={3.1f,7.62f,10.2f,11.18f};
                static const TCHAR* Names[]={TEXT("08-Loop"),TEXT("09-Waning"),TEXT("10-Seal"),TEXT("11-Afterglow")};
                int32 Id=Step-10;
                if (Cue.X<Ages[Id]) return false;
                if (Step==10) {CheckOrbit(Wind,TEXT("Runes"));CheckOrbit(Wind,TEXT("Runes001"));}
                if (Step==11) CheckOrbit(Wind,TEXT("Runes001"));
                if (Step==11) Test.TestTrue(TEXT("Native fade has started after 6.5 s"),Cue.Y<1. && Cue.Y>0.);
                if (Step==12) Test.TestTrue(TEXT("Seal survives the plume and glyphs"),Cue.Y==0. && Cue.Z==0. && Cue.W>0.);
                if (Step==13) Test.TestTrue(TEXT("Last contact glow fades continuously"),Cue.W<.1f && Cue.W>0.f);
                Shot(Names[Id]);break;
            }
            case 14:
                if(Wind->IsActive()) return false;
                Test.TestTrue(TEXT("Real-time wind cleanup preserves the two turn lift"),G->Board().Box.Z==1 && G->Board().Wind==2 && G->Board().SP==5);
                Shot(TEXT("12-Cleared"));break;
            case 15:
                PC->SetViewTarget(G);if (Observer.IsValid()) Observer->Destroy();
                if (PC->GetHUD()) PC->GetHUD()->bShowHUD=WasHUDVisible;
                G->Undo();
                Test.TestTrue(TEXT("Undo restores the uncast initial board"),G->Board().Wind==0 && G->Board().SP==8 && !Wind->IsActive() && !Range->IsActive());
                Captures.Insert(TEXT("frame,age_s,body,runes,seal,SP,wind_turns"),0);
                if (FParse::Param(FCommandLine::Get(),TEXT("RuneScreens")))
                {
                    for (const auto& Frame : NamedOpeningFrames)
                        Test.TestEqual(TEXT("Retain the opening frame that shares a named stage"),
                            IFileManager::Get().Copy(*Frame.Value,*Frame.Key),uint32(COPY_OK));
                    FFileHelper::SaveStringArrayToFile(Captures,*(Dir/TEXT("Phases.csv")));
                    FFileHelper::SaveStringArrayToFile(Counts,*(Dir/TEXT("EmitterCounts.csv")));
                }
                return true;
            }
            ++Step;return false;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAir02SequenceTest,"ResonantRunes.ShengLu.WindSequence",EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FAir02SequenceTest::RunTest(const FString&)
{
    for (const auto& C : GEngine->GetWorldContexts())
    {
        auto* World=C.World();
        if (!World || !World->IsGameWorld()) continue;
        auto* PC=World->GetFirstPlayerController();
        if (auto* G=PC?Cast<AShengLuBoard>(PC->GetPawn()):nullptr)
        {
            FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShareable(new FWindSequence(*this,G)));
            return true;
        }
    }
    AddError(TEXT("Start the ShengLu game world"));return false;
}
#endif
