// Copyright 2014, Andrew Scheidecker. All Rights Reserved. 

#include "BrickTerrainGenerationPluginPrivatePCH.h"
#include "BrickTerrainGenerationLibrary.h"
#include "BrickGridComponent.h"
#include "SimplexNoise.h"

UBrickTerrainGenerationLibrary::UBrickTerrainGenerationLibrary( const FPostConstructInitializeProperties& PCIP )
: Super( PCIP )
{}

class FLocalNoiseFunction : public FNoiseFunction
{
public:

	FLocalNoiseFunction(const FNoiseFunction& NoiseFunction,float LocalToWorldScale)
	: FNoiseFunction(NoiseFunction)
	, LocalToPeriodicScale(LocalToWorldScale / PeriodDistance)
	{
		SimplexNoiseModule = &ISimplexNoise::Get();

		// Compute the total denominator for all octaves of the noise.
		// Sum[l^i,{i,0,n-1}] == (L^n - 1) / (L - 1)
		const float ValueDenominator = (FMath::Pow(Lacunarity,OctaveCount) - 1.0f) / (Lacunarity - 1.0f);

		ValueScale = (MaxValue - MinValue) / (ValueDenominator * 2.0f);
		ValueBias = (MaxValue + MinValue) / 2.0f;
	}

	float Sample2D(float X,float Y) const
	{
		float ScaledX = X * LocalToPeriodicScale;
		float ScaledY = Y * LocalToPeriodicScale;
		float Value = 0.0f;
		for(int32 OctaveIndex = 0;OctaveIndex < OctaveCount;++OctaveIndex)
		{
			const float OctaveValue = SimplexNoiseModule->Sample2D(ScaledX,ScaledY);
			Value *= Lacunarity;
			Value += Ridged ? (-1.0f + 2.0f * (1.0f - FMath::Abs(OctaveValue))) : OctaveValue;
			ScaledX *= Lacunarity;
			ScaledY *= Lacunarity;
		}
		return Value * ValueScale + ValueBias;
	}

	float Sample3D(float X,float Y,float Z) const
	{
		float ScaledX = X * LocalToPeriodicScale;
		float ScaledY = Y * LocalToPeriodicScale;
		float ScaledZ = Z * LocalToPeriodicScale;
		float Value = 0.0f;
		for(int32 OctaveIndex = 0;OctaveIndex < OctaveCount;++OctaveIndex)
		{
			const float OctaveValue = SimplexNoiseModule->Sample3D(ScaledX,ScaledY,ScaledZ);
			Value *= Lacunarity;
			Value += Ridged ? (-1.0f + 2.0f * (1.0f - FMath::Abs(OctaveValue))) : OctaveValue;
			ScaledX *= Lacunarity;
			ScaledY *= Lacunarity;
			ScaledZ *= Lacunarity;
		}
		return Value * ValueScale + ValueBias;
	}

private:

	ISimplexNoise* SimplexNoiseModule;

	float LocalToPeriodicScale;
	float ValueScale;
	float ValueBias;
};

void UBrickTerrainGenerationLibrary::InitRegion(const FBrickTerrainGenerationParameters& Parameters,class UBrickGridComponent* Grid,const FInt3& RegionCoordinates)
{
	const double StartTime = FPlatformTime::Seconds();

	const float BiasedSeed = Parameters.Seed + 3.14159265359f;
	const float LocalToWorldScale = Grid->GetComponentScale().GetAbs().GetMin();
	const FLocalNoiseFunction LocalUnerodedHeightFunction(Parameters.UnerodedHeightFunction,LocalToWorldScale / Parameters.Scale);
	const FLocalNoiseFunction LocalErodedHeightFunction(Parameters.ErodedHeightFunction,LocalToWorldScale / Parameters.Scale);
	const FLocalNoiseFunction LocalErosionFunction(Parameters.ErosionFunction,LocalToWorldScale / Parameters.Scale);
	const FLocalNoiseFunction LocalMoistureFunction(Parameters.MoistureFunction,LocalToWorldScale / Parameters.Scale);
	const FLocalNoiseFunction LocalDirtThicknessFunction(Parameters.DirtThicknessFunction,LocalToWorldScale / Parameters.Scale);
	const FLocalNoiseFunction LocalCavernProbabilityFunction(Parameters.CavernProbabilityFunction,LocalToWorldScale / Parameters.Scale);
	const float NoiseToLocalScale = Parameters.Scale / LocalToWorldScale;

	// Allocate a local array for the generated bricks.
	const FInt3 BricksPerRegion = Grid->BricksPerRegion;
	TArray<uint8> LocalBrickMaterials;
	LocalBrickMaterials.Init(BricksPerRegion.X * BricksPerRegion.Y * BricksPerRegion.Z);

	const FInt3 MinRegionBrickCoordinates = RegionCoordinates * BricksPerRegion;
	const FInt3 MaxRegionBrickCoordinates = MinRegionBrickCoordinates + BricksPerRegion - FInt3::Scalar(1);
	for(int32 LocalY = 0;LocalY < BricksPerRegion.Y;++LocalY)
	{
		for(int32 LocalX = 0;LocalX < BricksPerRegion.X;++LocalX)
		{
			const int32 X = MinRegionBrickCoordinates.X + LocalX;
			const int32 Y = MinRegionBrickCoordinates.Y + LocalY;
			const float Erosion = LocalErosionFunction.Sample2D(BiasedSeed * 59 + X,Y);
			const float UnerodedGroundHeight = LocalUnerodedHeightFunction.Sample2D(BiasedSeed * 67 + X,Y) * NoiseToLocalScale;
			const float ErodedGroundHeight = LocalErodedHeightFunction.Sample2D(BiasedSeed * 71 + X,Y) * NoiseToLocalScale;
			const float RockHeight = FMath::Lerp(UnerodedGroundHeight,ErodedGroundHeight,Erosion);
			const float BaseDirtThickness = LocalDirtThicknessFunction.Sample2D(BiasedSeed * 79 + X,Y) * NoiseToLocalScale;
			const float DirtThicknessFactor = Parameters.DirtThicknessFactorByHeight->FloatCurve.Eval(RockHeight * LocalToWorldScale);
			const float DirtThickness = FMath::Max(0.0f,BaseDirtThickness * DirtThicknessFactor);
			const float GroundHeight = RockHeight + DirtThickness;

			const int32 BrickRockHeight = FMath::Min(MaxRegionBrickCoordinates.Z,FMath::Ceil(RockHeight));
			const int32 BrickGroundHeight = FMath::Min(MaxRegionBrickCoordinates.Z,FMath::Ceil(GroundHeight));

			float CavernProbabilitySamples[BrickGridConstants::MaxBricksPerRegionAxis];
			const uint32 NumCavernProbabilitySamples = FMath::Min(BrickGridConstants::MaxBricksPerRegionAxis >> 2,(BrickGroundHeight + 3) >> 2);
			float PreviousCavernProbabilitySample = LocalCavernProbabilityFunction.Sample3D(BiasedSeed * 73 + X,Y,MinRegionBrickCoordinates.Z - 1);
			for(uint32 CavernSampleIndex = 0;CavernSampleIndex < NumCavernProbabilitySamples;++CavernSampleIndex)
			{
				const uint32 LocalZ = CavernSampleIndex << 2;
				const float NextCavernProbabilitySample = LocalCavernProbabilityFunction.Sample3D(BiasedSeed * 73 + X,Y,MinRegionBrickCoordinates.Z + LocalZ + 3);
				CavernProbabilitySamples[LocalZ + 0] = FMath::Lerp(PreviousCavernProbabilitySample,NextCavernProbabilitySample,1.0f / 4.0f);
				CavernProbabilitySamples[LocalZ + 1] = FMath::Lerp(PreviousCavernProbabilitySample,NextCavernProbabilitySample,2.0f / 4.0f);
				CavernProbabilitySamples[LocalZ + 2] = FMath::Lerp(PreviousCavernProbabilitySample,NextCavernProbabilitySample,3.0f / 4.0f);
				CavernProbabilitySamples[LocalZ + 3] = NextCavernProbabilitySample;
				PreviousCavernProbabilitySample = NextCavernProbabilitySample;
			}

			for(int32 LocalZ = 0;LocalZ < BricksPerRegion.Z;++LocalZ)
			{
				const int32 Z = MinRegionBrickCoordinates.Z + LocalZ;
				const FInt3 BrickCoordinates(X,Y,Z);
				int32 MaterialIndex = 0;
				if(Z == Grid->MinBrickCoordinates.Z)
				{
					MaterialIndex = Parameters.BottomMaterialIndex;
				}
				else if(Z <= BrickGroundHeight)
				{
					const float CavernProbability = CavernProbabilitySamples[LocalZ];
					const float RockCavernThreshold = Parameters.CavernThresholdByHeight->FloatCurve.Eval(Z * LocalToWorldScale);
					if(Z <= BrickRockHeight)
					{
						if(CavernProbability > RockCavernThreshold)
						{
							MaterialIndex = Parameters.RockMaterialIndex;
						}
					}
					else
					{
						if(CavernProbability > RockCavernThreshold + Parameters.DirtCavernThresholdBias)
						{
							MaterialIndex = Z == BrickGroundHeight && LocalMoistureFunction.Sample2D(BiasedSeed * 61 + X,Y) > Parameters.GrassMoistureThreshold
								? Parameters.GrassMaterialIndex
								: Parameters.DirtMaterialIndex;
						}
					}
				}
				LocalBrickMaterials[((LocalY * BricksPerRegion.X) + LocalX) * BricksPerRegion.Z + LocalZ] = MaterialIndex;
			}
		}
	}

	Grid->SetBrickMaterialArray(MinRegionBrickCoordinates,MaxRegionBrickCoordinates,LocalBrickMaterials);

	UE_LOG(LogStats,Log,TEXT("UBrickTerrainGenerationLibrary::InitRegion took %fms"),1000.0f * float(FPlatformTime::Seconds() - StartTime));
}
