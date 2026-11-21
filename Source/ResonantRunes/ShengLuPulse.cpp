#include "ShengLuGame.h"
#include "Camera/CameraComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/CurveTable.h"
#include "NiagaraComponent.h"
#include "ProceduralMeshComponent.h"

namespace
{
    // UV0 crosses and follows the ribbon. UV1 selects the discharge material layer.
    struct FPulseStrips
    {
        const FTransform LocalToWorld;
        TArray<FVector> Vertices;
        TArray<int32> Indices;
        TArray<FVector2D> UV, Layer, Controls;
        TArray<FLinearColor> Colors;

        explicit FPulseStrips(const UProceduralMeshComponent& Mesh) : LocalToWorld(Mesh.GetComponentTransform()) {}
        void Pair(FVector Center,FVector HalfWidth,float V,FLinearColor Tint,FVector2D Mode,bool Connect,FVector2D Params={0,0})
        {
            const int I=Vertices.Num();
            Vertices.Add(LocalToWorld.InverseTransformPosition(Center-HalfWidth));
            Vertices.Add(LocalToWorld.InverseTransformPosition(Center+HalfWidth));
            UV.Append({{0,V},{1,V}});
            Layer.Append({Mode,Mode});
            Controls.Append({Params,Params});
            Colors.Append({Tint,Tint});
            if (Connect) Indices.Append({I-2,I-1,I,I,I-1,I+1});
        }
        void Flare(FVector Center,FVector Right,FVector Up,FLinearColor Tint,float ContactCore=0.f)
        {
            Card(Center,Right,Up,Tint,{1,ContactCore});
        }
        void Card(FVector Center,FVector Right,FVector Up,FLinearColor Tint,FVector2D Mode,FVector2D Params={0,0})
        {
            Pair(Center-Up,Right,0,Tint,Mode,false,Params);
            Pair(Center+Up,Right,1,Tint,Mode,true,Params);
        }
        void Submit(UProceduralMeshComponent& Mesh,int32 Section)
        {
            if (!Indices.IsEmpty())
                Mesh.CreateMeshSection_LinearColor(Section,Vertices,Indices,{},UV,Layer,Controls,{},Colors,{},false,false);
            else if (auto* Old=Mesh.GetProcMeshSection(Section); Old && !Old->ProcIndexBuffer.IsEmpty())
                Mesh.ClearMeshSection(Section);
        }
    };

    float Noise(float T,float Clock,float Seed)
    {
        return FMath::PerlinNoise2D({T+Seed,Clock});
    }

    // Retain the same bends from birth to death. Forks can sample their parent's exact centerline.
    struct FDischargePath
    {
        FVector Knots[8];
        FDischargePath(FVector Origin,FVector Heading,FVector Across,FVector Normal,float Reach,FRandomStream& Random)
        {
            Knots[0]=Origin;
            float Bend=Random.FRandRange(-.15f,.15f);
            for (int I=1;I<8;++I)
            {
                const float U=I/7.f;
                Bend=FMath::Clamp(Bend+Random.FRandRange(-.78f,.78f),-.96f,.96f);
                const float Advance=I==6 && Random.FRand()<.42f?-.045f:Random.FRandRange(.09f,.21f);
                Knots[I]=Knots[I-1]+Reach*(Heading*Advance+Across*Bend*.19f)
                    +Normal*(FMath::Sin(PI*U)*Random.FRandRange(-.3f,1.6f));
            }
        }
        FVector At(float U) const
        {
            const float X=FMath::Clamp(U,0.f,1.f)*7;
            const int I=FMath::Min(6,FMath::FloorToInt(X));
            const FVector A=Knots[I], B=Knots[I+1];
            const FVector DA=(B-Knots[FMath::Max(I-1,0)])*.40, DB=(Knots[FMath::Min(I+2,7)]-A)*.40;
            return FMath::Lerp(FMath::Lerp(A,B,X-I),FMath::CubicInterp(A,DA,B,DB,double(X-I)),.64);
        }
    };

    float Sample(UCurveTable* Table,FName Row,float Time,float Fallback)
    {
        const FRealCurve* Curve=Table?Table->FindCurve(Row,TEXT("Electric01"),false):nullptr;
        return Curve?Curve->Eval(Time):Fallback;
    }

    // Each visible source shockwave owns an RGBA LUT and three FloatFromCurve DIs.
    struct FWaveCurves
    {
        FName Progress, Intensity, Fade, Channels[3];
        FWaveCurves(const TCHAR* Emitter,int ColorExport,int FirstFloat)
        {
            const FString Prefix=FString(TEXT("__INTERNAL__.NE_BasicEmitter_Shockwave"))+Emitter+TEXT(".");
            auto Row=[&](const TCHAR* Name,int Index)
            {
                return Prefix+Name+FString::Printf(TEXT(".UpdateScript_0.%d"),Index);
            };
            Progress=FName(Row(TEXT("FloatFromCurve.FloatCurve"),FirstFloat));
            Intensity=FName(Row(TEXT("FloatFromCurve001.FloatCurve"),FirstFloat+1));
            Fade=FName(Row(TEXT("FloatFromCurve002.FloatCurve"),FirstFloat+2));
            for (int I=0;I<3;++I)
                Channels[I]=FName(Row(TEXT("ColorFromCurve.ColorCurve"),ColorExport)+FString::Printf(TEXT(".%c"),TEXT("RGBA")[I]));
        }
        void Draw(FPulseStrips& Mesh,UCurveTable* Table,float Age,float Layer,float Radius,FVector Center,
            FVector X,FVector Y,float RGBGain,float Opacity) const
        {
            // The VM writes progress to DynamicMaterialParameter.x. SpriteSize passes through Update.
            // Keep HDR gain out of vertex color (the procedural mesh quantizes that channel to a byte).
            FLinearColor Tint(Sample(Table,Channels[0],Age,.32f),Sample(Table,Channels[1],Age,.42f),
                Sample(Table,Channels[2],Age,1.f),Sample(Table,Fade,Age,1.f-Age)*Opacity);
            Mesh.Card(Center,X*Radius,Y*Radius,Tint,{float(Layer),Age},
                {Sample(Table,Progress,Age,Age),RGBGain*Sample(Table,Intensity,Age,1.f-Age)});
        }
    };

    // Both burst modules have the same count gate. They differ in SpawnGroup and particle constants.
    struct FWaveGroup
    {
        float Life, MinDiameter, MaxDiameter, Gain;
    };
    void DrawWaveBursts(FPulseStrips& Mesh,UCurveTable* Table,const FWaveCurves& Curves,int Layer,
        FWaveGroup First,FWaveGroup Second,float Time,float Period,int32 Seed,float DiameterScale,
        FVector Center,FVector X,FVector Y,float Alpha)
    {
        if (Time<0.f) return;
        const int Last=Period>0.f?FMath::FloorToInt(Time/Period):0;
        const int Oldest=Period>0.f?FMath::Max(0,FMath::FloorToInt((Time-FMath::Max(First.Life,Second.Life))/Period)):0;
        for (int Burst=Oldest;Burst<=Last;++Burst)
        {
            const float Elapsed=Time-Burst*Period;
            for (int Group=0;Group<2;++Group)
            {
                const FWaveGroup& Params=Group==0?First:Second;
                if (Elapsed>=Params.Life) continue;
                FRandomStream Random(HashCombineFast(uint32(Seed),uint32(Layer+11*Group+29*Burst)));
                const float Radius=Random.FRandRange(Params.MinDiameter,Params.MaxDiameter)*DiameterScale*.5f;
                const float A=(Layer==1 && Burst==0 && Group==0)?0.f:Random.FRandRange(-PI,PI);
                float S,C;
                FMath::SinCos(&S,&C,A);
                Curves.Draw(Mesh,Table,Elapsed/Params.Life,Layer+.25f*Group,Radius,Center,
                    X*C+Y*S,Y*C-X*S,Params.Gain,Alpha);
            }
        }
    }

    // A strip's width must be perpendicular to its projected tangent, including
    // a horizontal beam or an arc curling back towards the caster.
    template <typename Path>
    void Filament(FPulseStrips& Mesh,FVector Eye,int32 Segments,float Radius,FLinearColor Tint,Path Position,FVector2D Mode={0,0})
    {
        FVector P=Position(0.f), Previous=P;
        for (int32 I=0;I<=Segments;++I)
        {
            const float T=float(I)/Segments;
            const FVector Next=Position(FMath::Min(1.f,float(I+1)/Segments));
            FVector Side=FVector::CrossProduct(Next-Previous,Eye-P).GetSafeNormal();
            const float Width=Radius*(.22f+.78f*FMath::Sqrt(FMath::Max(0.f,FMath::Sin(PI*T))));
            Mesh.Pair(P,Side*Width,T,Tint,Mode,I>0);
            Previous=P; P=Next;
        }
    }
}

void AShengLuBoard::FadeContact()
{
    if (!LiveContact) return;
    // BP_FPAS_GameEffect.FadeOut closes emission. Its particles retain the last transform.
    LiveContact->SetVariableBool(TEXT("User.IsActive"),false);
    LiveContact->SetVariableFloat(TEXT("User.ElectricBranchRate"),0);
    LiveContact->SetVariableVec3(TEXT("User.ElectricSurfaceVelocity"),FVector::ZeroVector);
    LiveContact->Deactivate();
    FadingContacts.Add({LiveContact.Get(),EffectTime+10.f});
    LiveContact=nullptr;
    ContactSurface.Reset();
}

void AShengLuBoard::DrawPulse()
{
    FadingContacts.RemoveAll([this](const FFadingContact& F)
    {
        auto* FX=F.FX.Get();
        if (!FX) return true;
        if (FX->IsComplete() || EffectTime>=F.Deadline) { FX->DestroyComponent(); return true; }
        return false;
    });
    if (BeamFadeUntil>=0 && EffectTime>=BeamFadeUntil)
    {
        Lightning->DeactivateImmediate();
        BeamArm->DeactivateImmediate();
        BeamFadeUntil=-1;
    }
    const bool Charging=Selected==ERuneAction::Pulse && CastPhase==ECastPhase::Start;
    const bool Firing=IsFiring();
    const bool Contact=Firing && AimHit.bBlockingHit && AimHit.GetComponent();
    const float Raise=Charging?FMath::Clamp(PhaseTime/ChargeSeconds,0.f,1.f):1.f;
    const float FingerFlash=Charging?FMath::SmoothStep(.83f,.96f,Raise):
        Firing?1.f-FMath::SmoothStep(.013f,.10f,PhaseTime):0.f;
    // The corona crosses the phase boundary without resetting the ring's noise field.
    const float PalmTime=Firing?ChargeSeconds+PhaseTime:PhaseTime;
    Eyes->PostProcessSettings.MotionBlurAmount=Charging?.49f*FMath::SmoothStep(0.f,.14f,Raise)*
        (1.f-FMath::SmoothStep(.49f,.8f,Raise)):0.f;
    const float Power=Charging?FMath::Lerp(.08f,1.f,FMath::SmoothStep(0.f,.96f,Raise)):Firing?1.f:
        Selected==ERuneAction::Pulse && bReleasedCharged && CastPhase==ECastPhase::End?FMath::Max(0.f,1.f-PhaseTime/.15f):0.f;
    const FVector Palm=HandAnchor(), Eye=Eyes->GetComponentLocation();
    const FVector Right=Eyes->GetRightVector(), Up=Eyes->GetUpVector(), Forward=Eyes->GetForwardVector();
    const FLinearColor Blue(.32f,.35f,1.f,Power);
    FPulseStrips Light(*Ribbons);
    FPulseStrips Wave(*Ribbons);
    FPulseStrips ImpactWaves(*Ribbons);
    struct FArcSegment { FVector A,B; };
    TArray<FArcSegment,TInlineAllocator<48>> ArcCuts;
    HandGlow->SetVisibility(Power>0);
    ContactGlow->SetVisibility(Contact);
    if (Contact)
    {
        const FVector N=AimHit.ImpactNormal;
        auto* Surface=AimHit.GetComponent();
        const auto ViewDepth=[&](const FVector& P){return FMath::Abs(FVector::DotProduct(P-Eye,Forward));};
        if (LiveContact && (ContactSurface!=Surface || FVector::DotProduct(ContactNormal,N)<.94f ||
            FMath::Abs(ViewDepth(AimPoint)-ViewDepth(LiveContact->GetComponentLocation()))>=50.f ||
            FVector::DistSquared(AimPoint,LiveContact->GetComponentLocation())>85.f*85.f))
            FadeContact();
        const FVector Previous=LiveContact?LiveContact->GetComponentLocation():AimPoint;
        const FVector Travel=AimPoint-Previous;
        const float Dt=GetWorld()->GetDeltaSeconds();
        const float SourceRate=Travel.SizeSquared()<=35.f*35.f?15.f:0.f;
        if (!LiveContact)
        {
            LiveContact=NewObject<UNiagaraComponent>(this);
            LiveContact->SetAutoActivate(false);
            LiveContact->SetAsset(PulseImpact->GetAsset());
            LiveContact->SetWorldLocationAndRotation(AimPoint,N.Rotation());
            LiveContact->RegisterComponent();
            LiveContact->SetVariableBool(TEXT("User.IsActive"),true);
            LiveContact->SetVariableFloat(TEXT("User.ElectricBranchRate"),SourceRate);
            LiveContact->SetVariableVec3(TEXT("User.ElectricSurfaceVelocity"),FVector::ZeroVector);
            LiveContact->Activate(true);
            ContactSurface=Surface;
            ContactNormal=N;
            BranchRemainder=0;
            ContactStart=EffectTime;
            NextPuff=EffectTime+.04f+1.f/8.5f;
        }
        LiveContact->SetWorldLocationAndRotation(AimPoint,N.Rotation());
        LiveContact->SetVariableFloat(TEXT("User.ElectricBranchRate"),SourceRate);
        LiveContact->SetVariableVec3(TEXT("User.ElectricSurfaceVelocity"),Dt>SMALL_NUMBER?Travel/Dt:FVector::ZeroVector);
        const float Count=BranchRemainder+Dt*SourceRate;
        const int Born=FMath::FloorToInt(Count);
        BranchRemainder=Count-Born;
        const auto& Xf=Surface->GetComponentTransform();
        for (int I=0;I<Born;++I)
        {
            const int Serial=BranchSerial++;
            const int Seed=HashCombineFast(uint32(WaveSeed),uint32(Serial));
            FRandomStream R(Seed);
            FVector U,V;
            N.FindBestAxisVectors(U,V);
            const float A=Serial*2.39996f+R.FRandRange(-.42f,.42f);
            const float Lag=(Born-1-I+BranchRemainder)/SourceRate;
            const FVector Origin=FMath::Lerp(AimPoint,Previous,FMath::Clamp(Lag/FMath::Max(Dt,SMALL_NUMBER),0.f,1.f))+N*1.1f;
            ContactArcs.Add({Surface,Xf.InverseTransformPosition(Origin),Xf.InverseTransformVectorNoScale(N),
                Xf.InverseTransformVectorNoScale(U*FMath::Cos(A)+V*FMath::Sin(A)),EffectTime-Lag,
                .30f,float(R.FRandRange(32.f,62.f)),Seed});
        }
        while (EffectTime>=NextPuff)
        {
            const float Birth=NextPuff;
            NextPuff+=1.f/8.5f;
            // BasicSmoke accumulates rate for 1.15 seconds per loop, then pauses for 40 ms.
            const float ActiveEnd=ContactStart+.04f+1.15f+1.19f*FMath::FloorToFloat((Birth-ContactStart)/1.19f);
            if (NextPuff>=ActiveEnd) NextPuff+=.04f;
            FVector U,V;
            N.FindBestAxisVectors(U,V);
            const int32 Serial=PuffSerial++;
            FRandomStream Random(HashCombineFast(uint32(WaveSeed),uint32(Serial)));
            const float Life=Random.FRandRange(1.1f,2.2f), Radius=.05f*Random.FRandRange(620.f,800.f);
            // One atlas puff per birth. Its lips peel apart along a live branch.
            const float Angle=Serial*2.39996f+Random.FRandRange(-.6f,.6f);
            FVector Dir=U*FMath::Cos(Angle)+V*FMath::Sin(Angle);
            if (ContactArcs.Num())
            {
                const auto& Arc=ContactArcs[ContactArcs.Num()-1-Random.RandRange(0,FMath::Min(2,ContactArcs.Num()-1))];
                if (Arc.Surface==Surface) Dir=Xf.TransformVectorNoScale(Arc.Radial);
            }
            ContactPuffs.Add({AimPoint+N*2.f,Dir,N,Birth,Life,Radius,Random.FRand()});
        }
    }
    else FadeContact();
    ContactArcs.RemoveAll([this](const FContactArc& A){return !A.Surface.IsValid() || EffectTime-A.Birth>=A.Life;});
    ContactPuffs.RemoveAll([this](const FContactPuff& Puff){return EffectTime-Puff.Birth>=Puff.Lifetime;});
    if (Power>0)
    {
        HandGlow->SetWorldLocation(Palm+Forward*(3+4*FingerFlash)-Right*FingerFlash);
        static const FName LightRamp(TEXT("__INTERNAL__.NE_BasicEmitter_Light.FloatFromCurve.FloatCurve.UpdateScript.21"));
        const float Core=Charging?Sample(ChargeCurves,LightRamp,Raise,Power):Power;
        HandGlow->SetIntensity((.17f*Power+Core)*(2.3f+.24f*FMath::Sin(PalmTime*37.f))+FingerFlash*10.f);
        const float Glow=Charging?FMath::SmoothStep(.24f,.72f,Raise):Power;
        const float Valley=Charging?FMath::SmoothStep(.62f,.84f,Raise):0.f;
        const float Size=4.1f+6.2f*Power-3.3f*Valley;
        const FLinearColor PalmBlue(.23f,.31f,1.f,Glow);
        Light.Flare(Palm,Right*Size,Up*Size,PalmBlue.CopyWithNewOpacity(Glow*(.36f+.58f*Core)*(1.f-.56f*Valley)));
        Light.Card(Palm,Right*(Size*2.15f),Up*(Size*.7f),PalmBlue.CopyWithNewOpacity(Glow*(1.f-.25f*Valley)),{5,PalmTime});
        if (Charging)
        {
            static const FName AbsorbSize(TEXT("__INTERNAL__.Minimal004.ScaleSpriteSize.Uniform Curve Sprite Scale.UpdateScript.14"));
            const float Span=10.f+6.f*Sample(ChargeCurves,AbsorbSize,Raise,1.f-Raise*.5f);
            const float Gather=FMath::SmoothStep(0.f,.16f,Raise)*(1.f-.94f*FMath::SmoothStep(.54f,.81f,Raise));
            Light.Card(Palm+Forward*1.8f,Right*Span,Up*(Span*.91f),Blue.CopyWithNewOpacity(Gather),{2,PalmTime});
        }
        if ((Charging && Raise>.43f) || Firing)
        {
            const float RingPower=Charging?FMath::SmoothStep(.43f,.65f,Raise):
                .14f+.29f*(1.f-FMath::SmoothStep(0.f,.08f,PhaseTime));
            const float Radius=6.1f+Power*2.5f;
            static const FName Twitch(TEXT("__INTERNAL__.NE_Ring.NM_LightningNoise.TwitchCurve.UpdateScript.41"));
            const float Step=FMath::FloorToFloat(PalmTime/.05f);
            const float Blend=Sample(ChargeCurves,Twitch,PalmTime-Step*.05f,0.f);
            auto Flicker=[&](float U,float Seed)
            {
                return FMath::Lerp(Noise(U,Step*.41f,Seed),Noise(U,(Step+1)*.41f,Seed),Blend);
            };
            auto Ring=[&](float T)
            {
                const float A=.32f+T*5.25f+.43f*FMath::Sin(PalmTime*8)+.19f*Flicker(T*7,14);
                const float R=Radius*(1.f+.77f*Flicker(T*8,8)+.13f*Flicker(T*29,41));
                return Palm+Right*(R*FMath::Cos(A)-Radius*.19f)+Up*(R*FMath::Sin(A))+
                    Forward*(R*(.26f+.36f*FMath::Sin(A*2+PalmTime)));
            };
            Filament(Light,Eye,66,.18f,Blue.CopyWithNewOpacity(RingPower),Ring,{6,PalmTime});
            static const FName Bones[]={TEXT("thumb_03_r"),TEXT("index_03_r"),TEXT("middle_03_r")};
            for (int32 Finger=0;Finger<3;++Finger)
            {
                const FVector Joint=Hands->GetSocketLocation(Bones[Finger]);
                const FVector Root=Finger==1?Hands->GetSocketLocation(Bones[2]):Palm;
                const float Blink=FMath::SmoothStep(.44f,.88f,.5f+.5f*FMath::Sin(PalmTime*30+Finger*2.3f));
                Filament(Light,Eye,20,.14f,Blue.CopyWithNewOpacity(RingPower*Blink),[&](float T)
                {
                    return FMath::Lerp(Root,Joint,T)+FMath::Sin(PI*T)*
                        (Up*(1.9f+Flicker(T*8,4+Finger)*2.1f)+Right*Flicker(T*7,13+Finger)*2.9f-Forward);
                },{6,PalmTime+Finger*.32f});
            }
        }
        if (Charging || (Firing && PhaseTime<.22f))
        {
            // Runes04 has eight vertical glyphs in its green channel. The finger mesh occludes them.
            static const FName RuneSize(TEXT("__INTERNAL__.Runes.ScaleSpriteSize.Uniform Curve Sprite Scale.UpdateScript.46"));
            const float RuneAge=FMath::Clamp((PalmTime-.372f)/.23f,0.f,1.f);
            const float RunePower=FMath::SmoothStep(.30f,.54f,RuneAge)*(1.f-FMath::SmoothStep(.64f,1.f,RuneAge));
            const float Scale=Sample(ChargeCurves,RuneSize,RuneAge,1.f);
            for (int I=0;I<3 && RunePower>0;++I)
            {
                static constexpr float Azimuth[]={2.54f,.96f,4.60f};
                static constexpr int Glyph[]={7,2,0};
                const float A=Azimuth[I]+RuneAge*.16f;
                const FVector P=Palm+Forward*(2.f+I*.7f)+(Right*FMath::Cos(A)+Up*FMath::Sin(A))*4.7f;
                const float Breakup=Firing?.83f+.15f*FMath::SmoothStep(0.f,.09f,PhaseTime):RuneAge*.42f;
                Light.Card(P,(Right-Up*.18f)*(1.36f*Scale),(Up+Right*.18f)*(1.6f*Scale),
                    FLinearColor(.46f,.75f,1.f,RunePower),{8,float(Glyph[I])+Breakup});
            }
        }
        if (Charging || Firing)
        {
            static const FName BirthLife(TEXT("__INTERNAL__.ArmParticle.FloatFromCurve.FloatCurve.SpawnScript.1"));
            static const FName BirthSize(TEXT("__INTERNAL__.ArmParticle.FloatFromCurve001.FloatCurve.SpawnScript.2"));
            static const FName SizeOverLife(TEXT("__INTERNAL__.ArmParticle.ScaleSpriteSize.Uniform Curve Sprite Scale.UpdateScript.4"));
            static const FName FingerR(TEXT("__INTERNAL__.ArmParticle.ColorFromCurve.ColorCurve.UpdateScript.0.R"));
            static const FName FingerG(TEXT("__INTERNAL__.ArmParticle.ColorFromCurve.ColorCurve.UpdateScript.0.G"));
            static const FName FingerBones[]={TEXT("thumb_03_r"),TEXT("index_03_r"),TEXT("middle_03_r"),TEXT("ring_03_r")};
            FVector Joints[4];
            for (int I=0;I<4;++I) Joints[I]=Hands->GetSocketLocation(FingerBones[I]);
            const FVector Wrist=Hands->GetSocketLocation(TEXT("hand_r"));
            const int Last=FMath::FloorToInt(PhaseTime*15.f);
            // Sparse foreground streaks follow the animated digits. The native ArmParticle reader
            // independently emits the full population on its original lower-arm socket.
            for (int Parent=FMath::Max(0,Last-19);Parent<=Last;++Parent)
            {
                FRandomStream R(HashCombineFast(uint32(WaveSeed),uint32(Parent+(Firing?719:0))));
                const float ParentLife=R.FRandRange(.15f,.20f);
                const int Finger=R.RandRange(0,3);
                const FVector Start=(Parent%4==0)?Wrist:Palm;
                const FVector Tip=Joints[Finger];
                for (int Child=0;Child<9;++Child)
                {
                    const float T=(Child+R.FRand())/9.f;
                    const float Born=Parent/15.f+T*ParentLife;
                    const float Life=.1f+Sample(ArmCurves,BirthLife,R.FRand(),.2f);
                    const float Width=.1f+.2f*Sample(ArmCurves,BirthSize,R.FRand(),.1f);
                    const float Angle=R.FRandRange(-PI,PI)+T*3.8f;
                    const float Reach=R.FRandRange(1.1f,3.2f);
                    const float Age=(PhaseTime-Born)/Life;
                    if (Age<0 || Age>=1) continue;
                    const FVector Across=Right*FMath::Cos(Angle+Age)+Up*FMath::Sin(Angle+Age);
                    const FVector Pos=FMath::Lerp(Start,Tip,T)+Across*Reach*(.32f+Age)+Forward*(.55f+Age);
                    const FVector Vel=(Tip-Start).GetSafeNormal()+Across*(.25f+Age);
                    const FVector Projected=(Vel-Forward*FVector::DotProduct(Vel,Forward)).GetSafeNormal();
                    const float W=Width*.70f*Sample(ArmCurves,SizeOverLife,Age,1.f-Age);
                    const FLinearColor Spark(Sample(ArmCurves,FingerR,Age,.39f),Sample(ArmCurves,FingerG,Age,.54f),1.f,
                        Power*FMath::Min(1.f,Age*22.f));
                    Light.Card(Pos,FVector::CrossProduct(Forward,Projected)*(W*.45f),Projected*W*(1.6f+1.4f*T),Spark,{12,Age+T});
                }
            }
        }
    }
    if (Firing)
    {
        const FVector BoltEnd=AimPoint;
        Lightning->SetVariablePosition(TEXT("User.CenterPosition"),Palm);
        Lightning->SetVariablePosition(TEXT("User.TargetPosition"),BoltEnd);
        const FVector Axis=(BoltEnd-Palm).GetSafeNormal();
        FVector Lateral=FVector::CrossProduct(Up,Axis).GetSafeNormal();
        const FVector Vertical=FVector::CrossProduct(Axis,Lateral);
        const float Ignition=FMath::SmoothStep(0.f,.07f,PhaseTime);
        // The first-person beam is viewed almost end-on. Limit its projected folds
        // by the palm's distance from the sight line to the receiver.
        const float Parallax=FVector::CrossProduct(Palm-Eye,(BoltEnd-Eye).GetSafeNormal()).Size();
        const float Bend=FMath::Min(float(FVector::Distance(Palm,BoltEnd))*.065f,Parallax*.65f)*(.48f+.52f*Ignition);
        static const FName BeamTwitch(TEXT("__INTERNAL__.Lightning.NM_LightningNoise.TwitchCurve.UpdateScript.35"));
        static const FName BeamWidth(TEXT("__INTERNAL__.Lightning.FloatFromCurve.FloatCurve.UpdateScript.34"));
        static const FName BeamImpulse(TEXT("__INTERNAL__.Lightning.FloatFromCurve005.FloatCurve.UpdateScript.40"));
        const float BeamTime=FMath::Max(0.f,PhaseTime-.04f);
        const float SampleTime=FMath::FloorToFloat(BeamTime/.05f);
        const float Crossfade=Sample(TrailCurves,BeamTwitch,BeamTime-SampleTime*.05f,0);
        const float Seed=float(uint32(WaveSeed)%701)*.13f;
        auto BendField=[&](float U,float Channel)
        {
            return FMath::Lerp(Noise(U,SampleTime*.48f,Seed+Channel),Noise(U,(SampleTime+1)*.48f,Seed+Channel),Crossfade);
        };
        auto CenterLine=[&](float T)
        {
            const float Shape=Bend*FMath::Sin(PI*T)*1.55f;
            return FMath::Lerp(Palm,BoltEnd,T)+Shape*(
                Lateral*(BendField(T*4.3f,2)+.12f*BendField(T*17,7))+
                Vertical*(BendField(T*4.1f,17)+.15f*BendField(T*16,13)));
        };
        static const FWaveCurves Outer(TEXT(""),12,64), ReturnFold(TEXT("002"),14,70);
        const FVector Cap=Palm+Forward*4.3f-Right*1.3f+Axis*FMath::Min(PhaseTime*10.f,3.f);
        DrawWaveBursts(Wave,TrailCurves,Outer,0,{.30f,18,28,11},{.50f,25,45,11},
            PhaseTime,.13f,WaveSeed,1,Cap,Right,Up,.2f);
        DrawWaveBursts(Wave,TrailCurves,ReturnFold,1,{.30f,60,60,12},{.45f,45,45,30},
            PhaseTime,0,WaveSeed,1,Cap+Forward*.8f-Up*.7f,Right*.98f-Up*.20f,Up*.98f+Right*.20f,.2f);
        if (PhaseTime<.30f)
        {
            const float Age=PhaseTime/.30f;
            const FVector Out=(Axis-Forward*FVector::DotProduct(Axis,Forward)).GetSafeNormal();
            const FVector Cross=FVector::CrossProduct(Forward,Out);
            const float Plume=1.f-FMath::SmoothStep(.10f,.56f,Age);
            if (Plume>0.f)
                Light.Card(Palm+Out*3.7f+Forward*1.5f,Out*6.1f,Cross*3.2f,Blue.CopyWithNewOpacity(Plume),{9,Age});
        }
        if (PhaseTime>=.04f)
        {
            const float Impulse=Sample(TrailCurves,BeamImpulse,BeamTime,1.f);
            Filament(Light,Eye,99,.60f+.38f*Ignition+.065f*Impulse,FLinearColor(.53f,.62f,1.f),CenterLine,{7,BeamTime});
            if (BeamTime>=.02f)
                for (int Group=1;Group<=2;++Group)
                {
                    const float Sign=Group==1?1.f:-1.f;
                    const float Split=(Group==1?.14f:.49f)+BendField(.5f,Group*4.f)*.11f;
                    const float Join=(Group==1?.81f:.97f)+BendField(.9f,Group*5.f)*.035f;
                    const float Wire=Group==1?.51f:.30f;
                    Filament(Light,Eye,99,Wire,Blue.CopyWithNewOpacity(Group==1?.96f:.73f),[&](float T)
                    {
                        const float Envelope=FMath::Sin(PI*T)*Sample(TrailCurves,BeamWidth,T,1.f);
                        return CenterLine(FMath::Lerp(Split,Join,T))+Bend*Envelope*Sign*
                            (Lateral*(.74f+.58f*BendField(T*6,30+Group))+Vertical*(.52f+.46f*BendField(T*9,40+Group)));
                    },{7,BeamTime+.19f*Group});
                }
        }
    }
    if (Contact)
    {
        const FVector N=AimHit.ImpactNormal;
        const FVector Center=AimPoint+N*.9f;
        const float HitAge=EffectTime-ContactStart;
        const float Arrival=FMath::SmoothStep(0.f,.115f,HitAge);
        ContactGlow->SetWorldLocation(Center+N*7);
        ContactGlow->SetIntensity((.12f+.88f*Arrival)*(3.2f+.85f*FMath::Sin(HitAge*39)));
        FVector Tangent, Bitangent;
        N.FindBestAxisVectors(Tangent,Bitangent);
        const float Burst=1.f-FMath::SmoothStep(.09f,.31f,HitAge);
        static const FWaveCurves Energy(TEXT(""),11,62), FaceRip(TEXT("002"),12,65), AirRip(TEXT("003"),13,68);
        // Scale the original impact sprite diameters to the one-metre box.
        constexpr float BoxScale=.1f;
        DrawWaveBursts(ImpactWaves,HitCurves,Energy,2,{.35f,930,930,8},{.60f,830,830,8},
            HitAge-.04f,.14f,WaveSeed,BoxScale,Center+N*4.f,Right,Up,.2f*Arrival);
        DrawWaveBursts(ImpactWaves,HitCurves,FaceRip,3,{.40f,750,750,25},{.70f,570,570,25},
            HitAge,.2f,WaveSeed,BoxScale,Center+N*2.2f,Tangent,Bitangent,Arrival);
        DrawWaveBursts(ImpactWaves,HitCurves,AirRip,4,{.35f,550,550,32},{.50f,400,400,32},
            HitAge,.2f,WaveSeed,BoxScale,Center+N*6.f,Right,Up,Arrival);
        const float Halo=FMath::Lerp(2.5f,28.f+Burst*9.f,Arrival);
        // Align the broad field to the receiver so the near half cannot clip into a sloping face.
        Light.Flare(Center+N*.8f,Tangent*Halo,Bitangent*Halo,FLinearColor(.32f,.38f,1.f),1.f);
        const float Flash=FMath::Lerp(2.f,8.f+4.f*Burst,Arrival);
        Light.Flare(Center+N*1.1f,Tangent*Flash,Bitangent*Flash,FLinearColor(.72f,.78f,1.f));
        static const FName HitTwitch(TEXT("__INTERNAL__.NE_Ring002.NM_LightningNoise.TwitchCurve.UpdateScript.103"));
        static const FName GrowCurve(TEXT("__INTERNAL__.Minimal.FloatFromCurve008.FloatCurve.UpdateScript.38"));
        static const FName TailCurve(TEXT("__INTERNAL__.Minimal.FloatFromCurve.FloatCurve.UpdateScript.42"));
        static const FName BranchWidth(TEXT("__INTERNAL__.Minimal.FloatFromCurve006.FloatCurve.SpawnScript.35"));
        static const FName ColorR(TEXT("__INTERNAL__.Minimal.ColorFromCurve.ColorCurve.UpdateScript.9.R"));
        static const FName ColorG(TEXT("__INTERNAL__.Minimal.ColorFromCurve.ColorCurve.UpdateScript.9.G"));
        for (const FContactArc& Arc:ContactArcs)
        {
            const auto& Xf=Arc.Surface->GetComponentTransform();
            const FVector Root=Xf.TransformPosition(Arc.Origin);
            const FVector Normal=Xf.TransformVectorNoScale(Arc.Normal), Radial=Xf.TransformVectorNoScale(Arc.Radial);
            const FVector Side=FVector::CrossProduct(Normal,Radial);
            const float Seconds=EffectTime-Arc.Birth, Age=FMath::Clamp(Seconds/Arc.Life,0.f,1.f);
            const float Head=Sample(HitCurves,GrowCurve,Seconds,Age);
            const float Fade=Sample(HitCurves,TailCurve,Age,1.f-Age);
            FRandomStream R(Arc.Seed);
            const FDischargePath Trunk(Root,Radial,Side,Normal,Arc.Reach,R);
            const float Phase=R.FRand(), Tick=FMath::FloorToFloat(Seconds/.05f);
            const float Twitch=Sample(HitCurves,HitTwitch,Seconds-Tick*.05f,0.f);
            auto Cable=[&](const FDischargePath& Path,float U)
            {
                const float J=FMath::Lerp(Noise(U*18,Tick,Phase*64),Noise(U*18,Tick+1,Phase*64),Twitch);
                return Path.At(U)+Side*(J*.92f*FMath::Sin(PI*U));
            };
            const FLinearColor Ion(Sample(HitCurves,ColorR,Age,.43f),Sample(HitCurves,ColorG,Age,.52f),1,
                Fade*(.28f+.72f*Arrival));
            auto Draw=[&](const FDischargePath& Path,float Progress,float Width,int Segments)
            {
                if (Progress<=.005f) return;
                auto P=[&](float U){return Cable(Path,U*Progress);};
                // Separate the diffuse return glow from the narrow, granular conducting channel.
                Filament(Light,Eye,Segments,Width*2.9f,Ion.CopyWithNewOpacity(.42f*Ion.A),P,{10,Seconds+Phase});
                Filament(Light,Eye,Segments,Width,Ion,P,{11,Seconds+Phase});
                if (Ion.A>.13f)
                    for(int J=0;J<7;++J) ArcCuts.Add({P(J/7.f),P((J+1)/7.f)});
            };
            const float WidthRoll=R.FRand();
            const float Thickness=.5f*BoxScale*(18.f+5.f*Sample(HitCurves,BranchWidth,WidthRoll,WidthRoll));
            Draw(Trunk,Head,Thickness,46);
            const int Forks=1+(Arc.Seed&1);
            for (int J=0;J<Forks;++J)
            {
                const float Joint=J==0?R.FRandRange(.30f,.53f):R.FRandRange(.60f,.79f);
                const float ForkAge=FMath::Clamp((Head-Joint)/(1.f-Joint),0.f,1.f);
                const float Angle=(J==0?1.f:-1.f)*R.FRandRange(.55f,1.34f);
                const FVector Dir=Radial*FMath::Cos(Angle)+Side*FMath::Sin(Angle);
                const FDischargePath Fork(Cable(Trunk,Joint),Dir,FVector::CrossProduct(Normal,Dir),Normal,
                    Arc.Reach*R.FRandRange(.38f,.70f),R);
                Draw(Fork,ForkAge,Thickness*(J==0?.64f:.39f),28);
                if (J==0 && (Arc.Seed&2) && ForkAge>.58f)
                {
                    const FDischargePath Tip(Cable(Fork,.58f),Side,Radial,Normal,Arc.Reach*.24f,R);
                    Draw(Tip,(ForkAge-.58f)/.42f,Thickness*.27f,14);
                }
            }
        }
        // Brief streaks separate from the hit, spreading over and away from the face.
        for (int I=0;I<16;++I)
        {
            const float Cycle=HitAge*2.5f+I*.618034f;
            const float Age=FMath::Frac(Cycle);
            const float A=I*2.4f+FMath::FloorToFloat(Cycle)*1.31f;
            const FVector Dir=Tangent*FMath::Cos(A)+Bitangent*FMath::Sin(A);
            const FVector Velocity=Dir*(27.f+11.f*FMath::Sin(I*3.1f))+N*(7.f+I*.87f);
            const FVector P=Center+Velocity*Age-FVector::UpVector*(Age*Age*3);
            const FVector Side=FVector::CrossProduct(Velocity,Eye-P).GetSafeNormal()*.16f;
            const FLinearColor Tint(.44f,.59f,1.f,FMath::Sin(PI*Age)*.85f*Arrival);
            Light.Pair(P-Velocity*.026f,Side*.25f,0,Tint,{0,0},false);
            Light.Pair(P,Side,1,Tint,{0,0},true);
        }
    }
    // BasicSmoke owns the visible Smoke02 renderer. ImpactSmoke has no serialized renderer.
    static const FName SmokeSize(TEXT("__INTERNAL__.NE_BasicEmitter_BasicSmoke.ScaleSpriteSize.Uniform Curve Sprite Scale.UpdateScript_5.48"));
    static const FName SmokeFlash(TEXT("__INTERNAL__.NE_BasicEmitter_BasicSmoke.FloatFromCurve005.FloatCurve.UpdateScript_5.49"));
    static const FName SmokeSpread(TEXT("__INTERNAL__.NE_BasicEmitter_BasicSmoke.FloatFromCurve002.FloatCurve.UpdateScript_5.50"));
    static const FName SmokeThickness(TEXT("__INTERNAL__.NE_BasicEmitter_BasicSmoke.FloatFromCurve003.FloatCurve.UpdateScript_5.51"));
    static const FName SmokeErosion(TEXT("__INTERNAL__.NE_BasicEmitter_BasicSmoke.FloatFromCurve004.FloatCurve.UpdateScript_5.52"));
    static const FName SmokeFrame(TEXT("__INTERNAL__.NE_BasicEmitter_BasicSmoke.FloatFromCurve.FloatCurve.UpdateScript_5.53"));
    static const FName SmokeEnvelope(TEXT("__INTERNAL__.NE_BasicEmitter_BasicSmoke.FloatFromCurve001.FloatCurve.UpdateScript_5.54"));
    for (const FContactPuff& Puff : ContactPuffs)
    {
        const float Age=(EffectTime-Puff.Birth)/Puff.Lifetime;
        const float Open=Sample(HitCurves,SmokeFrame,Age,Age);
        const float Size=Puff.Radius*Sample(HitCurves,SmokeSize,Age,.6f+.4f*Age);
        const float Spread=Sample(HitCurves,SmokeSpread,Age,1.f);
        const float Thickness=Sample(HitCurves,SmokeThickness,Age,1.f);
        const float Edge=Sample(HitCurves,SmokeErosion,Age,1.f-Age);
        const float Flash=Sample(HitCurves,SmokeFlash,Age,0.f);
        const float Fade=Sample(HitCurves,SmokeEnvelope,Age,1-Age);
        const FVector Across=FVector::CrossProduct(Puff.Normal,Puff.Radial);
        const FLinearColor Tint(Thickness,Spread,Flash,Fade);
        // The two lips leave a clear channel around the same fork that launched the smoke.
        for (float Side : {-1.f,1.f})
        {
            // Sample the middle of the sheet as well as the silhouette: a branch can cut
            // through its interior without ever passing either outside edge.
            constexpr int AcrossSteps=5, AlongSteps=13;
            for (int I=0;I<=AlongSteps;++I)
            {
                const float V=float(I)/AlongSteps;
                const float Curl=V*(2.12f+Open*1.6f+Puff.Roll*.87f)+Side*(.13f+.23f*Puff.Roll)*FMath::Sin(V*PI);
                const float Reach=Size*(.17f+.58f*Open+.62f*V+.52f*FMath::Sin(Curl));
                const FVector Center=Puff.Origin+Puff.Radial*Reach+
                    Across*(Side*(2.f+Size*(.19f+.18f*Open)*(1.f-FMath::Cos(Curl))))+
                    Puff.Normal*(1.5f+Size*((.16f+.22f*Open)*FMath::Sin(Curl)+.18f*V*V));
                const FVector Width=(Across*(Side*FMath::Cos(Curl))+Puff.Normal*FMath::Sin(Curl))*
                    (Size*Spread*Thickness*(.10f+.39f*FMath::Sin(V*PI)));
                for (int J=0;J<=AcrossSteps;++J)
                {
                    const float U=float(J)/AcrossSteps, X=2.f*U-1.f;
                    const float Fold=FMath::Sin(PI*U)*FMath::Sin(PI*V);
                    const FVector P=Center+Width*X+Puff.Normal*(Size*Fold*.13f*FMath::Sin(Curl+X*1.7f));
                    float DistSq=64.f;
                    for (const auto& Cut : ArcCuts)
                        DistSq=FMath::Min(DistSq,float(FMath::PointDistToSegmentSquared(P,Cut.A,Cut.B)));
                    const float CutMask=1.f-FMath::SmoothStep(1.5f,8.f,FMath::Sqrt(DistSq));
                    const int K=Light.Vertices.Num(),Previous=K-(AcrossSteps+1);
                    Light.Vertices.Add(Light.LocalToWorld.InverseTransformPosition(P+Puff.Normal*(5.f*CutMask*(1.f-CutMask))));
                    Light.UV.Add({U,V});
                    Light.Layer.Add({3,Open});
                    Light.Controls.Add({Edge,CutMask});
                    Light.Colors.Add(Tint);
                    if (I>0 && J>0) Light.Indices.Append({Previous-1,Previous,K-1,K-1,Previous,K});
                }
            }
        }
    }
    Light.Submit(*Ribbons,1);
    Wave.Submit(*Ribbons,3);
    ImpactWaves.Submit(*Ribbons,4);
    UpdateScars();
}

void AShengLuBoard::UpdateScars()
{
    constexpr float Lifetime=1.6f, Spacing=5.f, MaxSpan=85.f;
    Scars.RemoveAll([&](const FScarPoint& P){ return !P.Surface.IsValid() || EffectTime-P.Birth>=Lifetime; });
    auto* Surface=IsFiring() && AimHit.bBlockingHit?AimHit.GetComponent():nullptr;
    if (!Surface)
    {
        LastScarSample.Reset();
        bStrokeStarted=false;
    }
    else
    {
        const FTransform& Xf=Surface->GetComponentTransform();
        const FVector Local=Xf.InverseTransformPosition(AimHit.ImpactPoint);
        const FVector Normal=(Xf.InverseTransformVectorNoScale(AimHit.ImpactNormal)*Xf.GetScale3D()).GetSafeNormal();
        FScarPoint Sample{Surface,Local,Normal,EffectTime,0,0,true};
        const float Travel=LastScarSample && LastScarSample->Surface==Surface?
            Xf.TransformVector(Local-LastScarSample->Position).Size():0.f;
        const FVector PreviousWorld=LastScarSample?Xf.TransformPosition(LastScarSample->Position):AimPoint;
        const auto Depth=[&](FVector World){return FMath::Abs(FVector::DotProduct(World-Eyes->GetComponentLocation(),Eyes->GetForwardVector()));};
        const float Dt=GetWorld()->GetDeltaSeconds();
        const float Speed=Dt>SMALL_NUMBER?Travel/Dt:0.f;
        const bool NewFace=!LastScarSample || LastScarSample->Surface!=Surface || FMath::Abs(Depth(PreviousWorld)-Depth(AimPoint))>=50.f ||
            FVector::DotProduct(LastScarSample->Normal,Normal)<.94f || Travel>MaxSpan || Speed>5000.f;
        if (NewFace)
        {
            LastScarSample=Sample;
            ScarRemainder=0;
            bStrokeStarted=false;
        }
        else if (Travel>KINDA_SMALL_NUMBER)
        {
            const auto Previous=LastScarSample.GetValue();
            // A paused head remains an anchor but does not keep depositing particles.
            // Resume with a fresh head if the old ribbon has already cooled away.
            if (EffectTime-Previous.Birth>=Lifetime)
            {
                bStrokeStarted=false;
                ScarRemainder=0;
            }
            const float Total=ScarRemainder+Travel;
            const int32 Count=FMath::FloorToInt(Total/Spacing);
            if (Count && !bStrokeStarted)
            {
                FScarPoint Head=Previous;
                Head.Birth=EffectTime;
                Head.bStart=true;
                Scars.Add(Head);
                bStrokeStarted=true;
            }
            Sample.Speed=FMath::Clamp(Speed/1500.f,0.f,1.f);
            Sample.Distance=Previous.Distance+Travel;
            for (int32 I=1;I<=Count;++I)
            {
                const float T=(I*Spacing-ScarRemainder)/Travel;
                FScarPoint P=Sample;
                P.Position=FMath::Lerp(Previous.Position,Local,T);
                P.Normal=FMath::Lerp(Previous.Normal,Normal,T).GetSafeNormal();
                P.Distance=Previous.Distance+Travel*T;
                P.Birth=FMath::Lerp(FMath::Max(Previous.Birth,EffectTime-Dt),EffectTime,T);
                P.bStart=false;
                Scars.Add(P);
            }
            ScarRemainder=Total-Count*Spacing;
            LastScarSample=Sample;
            constexpr int32 Budget=2048;
            if (Scars.Num()>Budget) Scars.RemoveAt(0,Scars.Num()-Budget,EAllowShrinking::No);
        }
    }

    FPulseStrips Mesh(*Ribbons);
    if (!Scars.IsEmpty())
    {
        // Hit.Minimal004's recovered curve table stores the four channels independently.
        static const FName Rows[]={
            TEXT("__INTERNAL__.Minimal004.ColorFromCurve.ColorCurve.UpdateScript.10.R"),
            TEXT("__INTERNAL__.Minimal004.ColorFromCurve.ColorCurve.UpdateScript.10.G"),
            TEXT("__INTERNAL__.Minimal004.ColorFromCurve.ColorCurve.UpdateScript.10.B"),
            TEXT("__INTERNAL__.Minimal004.ColorFromCurve.ColorCurve.UpdateScript.10.A")};
        const FRealCurve* Channels[4]={};
        static const FName CoolingRow(TEXT("__INTERNAL__.Minimal004.FloatFromCurve001.FloatCurve.UpdateScript.45"));
        static const FName OpacityRow(TEXT("__INTERNAL__.Minimal004.FloatFromCurve.FloatCurve.UpdateScript.46"));
        static const FName DissolveRow(TEXT("__INTERNAL__.Minimal004.FloatFromCurve002.FloatCurve.UpdateScript.47"));
        if (HitCurves)
            for (int C=0;C<4;++C) Channels[C]=HitCurves->FindCurve(Rows[C],TEXT("Electric01 surface ribbon"),false);
        for (int I=0;I<Scars.Num();++I)
        {
            const auto& P=Scars[I];
            const bool Prev=I>0 && !P.bStart && Scars[I-1].Surface==P.Surface;
            const bool Next=I+1<Scars.Num() && !Scars[I+1].bStart && Scars[I+1].Surface==P.Surface;
            if (!Prev && !Next) continue;
            const auto& Xf=P.Surface->GetComponentTransform();
            const FVector A=Prev?Scars[I-1].Position:P.Position;
            const FVector B=Next?Scars[I+1].Position:P.Position;
            const FVector N=Xf.TransformVectorNoScale(P.Normal*FTransform::GetSafeScaleReciprocal(Xf.GetScale3D())).GetSafeNormal();
            const FVector Tangent=Xf.TransformVector(B-A).GetSafeNormal();
            const float Width=.05f*FMath::Lerp(75.f,45.f,P.Speed)*(Prev && Next?1.f:.22f);
            const FVector Edge=FVector::CrossProduct(N,Tangent).GetSafeNormal()*Width;
            const float Seconds=EffectTime-P.Birth;
            const float Age=FMath::Clamp(Seconds/Lifetime,0.f,1.f);
            FLinearColor Tint(.447f,.571f,1.f,1.f-Age);
            for (int C=0;C<4;++C) if (Channels[C]) Tint.Component(C)=Channels[C]->Eval(Age);
            const float Heat=Sample(HitCurves,CoolingRow,Age,1-Age)*FMath::Lerp(500.f,0.f,FMath::Sqrt(P.Speed));
            Tint.A=Sample(HitCurves,OpacityRow,Age,1-Age)*FMath::Lerp(1.4f,.2f,P.Speed);
            Mesh.Pair(Xf.TransformPosition(P.Position)+N*.23f,Edge,P.Distance/15.f,Tint,{-2,Age},Prev,
                {Heat,Sample(HitCurves,DissolveRow,Age,1-Age)});
        }
    }
    Mesh.Submit(*Ribbons,2);
}
