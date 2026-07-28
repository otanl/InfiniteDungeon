#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "InfiniteDungeonPawn.generated.h"

class UCameraComponent;
class UStaticMesh;
class UMaterialInterface;
class USphereComponent;
class UInstancedStaticMeshComponent;

/**
 * Endless dungeon you can walk through, generated as a 2D grid of weighted-WFC
 * chunks around the player. Each chunk opens at the middle of all four edges so
 * neighbours always connect; chunks within a radius of the player are streamed
 * in and distant ones destroyed. Two modes (Tab toggles): auto-explore, or
 * manual WASD + mouse walking with wall collision and floor following.
 */
UCLASS()
class INFINITEDUNGEON_API AInfiniteDungeonPawn : public APawn
{
	GENERATED_BODY()

public:
	AInfiniteDungeonPawn();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

	// --- layout ---
	UPROPERTY(EditAnywhere, Category = "Dungeon") float CellSize = 700.f;
	UPROPERTY(EditAnywhere, Category = "Dungeon") float WallHeight = 600.f;
	UPROPERTY(EditAnywhere, Category = "Dungeon") float CameraHeight = 250.f;
	UPROPERTY(EditAnywhere, Category = "Dungeon") int32 ChunkCols = 7;
	UPROPERTY(EditAnywhere, Category = "Dungeon") int32 ChunkRows = 7;
	UPROPERTY(EditAnywhere, Category = "Dungeon") int32 RoomsPerChunk = 1;
	UPROPERTY(EditAnywhere, Category = "Dungeon") bool bCeiling = true;
	UPROPERTY(EditAnywhere, Category = "Dungeon") bool bDoorways = true;
	UPROPERTY(EditAnywhere, Category = "Dungeon") float DoorHeight = 380.f;
	UPROPERTY(EditAnywhere, Category = "Dungeon") float DoorWidth = 280.f;
	UPROPERTY(EditAnywhere, Category = "Dungeon") float PillarRoomChance = 0.5f;
	UPROPERTY(EditAnywhere, Category = "Dungeon") float PillarRadius = 2.0f;

	/** How many chunks around the player to keep generated (radius in chunks). */
	UPROPERTY(EditAnywhere, Category = "Dungeon") int32 LoadRadius = 2;

	/** How many chunks to build (and retire) per frame while streaming. Higher fills in faster but costs more per frame. */
	UPROPERTY(EditAnywhere, Category = "Dungeon") int32 ChunksPerTick = 1;

	/** Vertical spacing between stacked dungeon layers (must exceed WallHeight). */
	UPROPERTY(EditAnywhere, Category = "Dungeon") float LayerGap = 720.f;

	/** Roughly one chunk in this many has a stairway up to the next layer (higher = rarer). */
	UPROPERTY(EditAnywhere, Category = "Dungeon") int32 StairRarity = 5;

	/** Surface material for floors, walls and ceilings (defaults to plain white). */
	UPROPERTY(EditAnywhere, Category = "Dungeon") UMaterialInterface* SurfaceMaterial = nullptr;

	// --- lighting ---
	UPROPERTY(EditAnywhere, Category = "Dungeon|Lighting") int32 LightEveryCells = 3;
	UPROPERTY(EditAnywhere, Category = "Dungeon|Lighting") float LightIntensity = 5000.f;
	UPROPERTY(EditAnywhere, Category = "Dungeon|Lighting") float LightRadius = 1400.f;
	UPROPERTY(EditAnywhere, Category = "Dungeon|Lighting") FLinearColor LightColor = FLinearColor(1.f, 0.82f, 0.55f);
	UPROPERTY(EditAnywhere, Category = "Dungeon|Lighting") UStaticMesh* BulbMesh = nullptr;
	UPROPERTY(EditAnywhere, Category = "Dungeon|Lighting") UMaterialInterface* BulbMaterial = nullptr;
	UPROPERTY(EditAnywhere, Category = "Dungeon|Lighting") float BulbSize = 0.35f;
	UPROPERTY(EditAnywhere, Category = "Dungeon|Lighting") float BulbCeilingDrop = -1.f;

	// --- camera shake ---
	UPROPERTY(EditAnywhere, Category = "Dungeon|Camera") bool bCameraShake = true;
	UPROPERTY(EditAnywhere, Category = "Dungeon|Camera") float ShakePosAmount = 0.8f;
	UPROPERTY(EditAnywhere, Category = "Dungeon|Camera") float ShakeRotAmount = 0.3f;
	UPROPERTY(EditAnywhere, Category = "Dungeon|Camera") float ShakeSpeed = 1.0f;
	UPROPERTY(EditAnywhere, Category = "Dungeon|Camera") float ShakeJitter = 0.0f;
	UPROPERTY(EditAnywhere, Category = "Dungeon|Camera") float WalkBobAmount = 1.8f;
	UPROPERTY(EditAnywhere, Category = "Dungeon|Camera") float WalkStride = 440.f;
	UPROPERTY(EditAnywhere, Category = "Dungeon|Camera") float WalkBobRoll = 0.22f;

	// --- control ---
	UPROPERTY(EditAnywhere, Category = "Dungeon|Control") bool bManualMode = false;
	UPROPERTY(EditAnywhere, Category = "Dungeon|Control") float MoveSpeed = 240.f;
	UPROPERTY(EditAnywhere, Category = "Dungeon|Control") float ManualSpeed = 400.f;
	UPROPERTY(EditAnywhere, Category = "Dungeon|Control") float TurnSpeed = 90.f;
	UPROPERTY(EditAnywhere, Category = "Dungeon|Control") float LookSensitivity = 2.5f;
	UPROPERTY(EditAnywhere, Category = "Dungeon|Control") float CollisionRadius = 40.f;

private:
	UPROPERTY() USphereComponent* Collision = nullptr;
	UPROPERTY() UCameraComponent* Cam = nullptr;
	UPROPERTY() UStaticMesh* FloorMesh = nullptr;
	UPROPERTY() UStaticMesh* WallMesh = nullptr;
	UPROPERTY() UStaticMesh* PillarMesh = nullptr;

	// Spawned actors grouped by chunk coord (x, y, layer), so distant chunks can be destroyed.
	TMap<FIntVector, TArray<TObjectPtr<AActor>>> LoadedChunks;
	TArray<TObjectPtr<AActor>>* CurrentChunkList = nullptr;
	FIntVector LastPlayerChunk = FIntVector(MAX_int32, MAX_int32, MAX_int32);

	// Chunks waiting to be built, nearest first - drained a few per frame.
	TArray<FIntVector> PendingChunks;

	// Scratch state while building one chunk: every piece of geometry becomes an
	// instance on a per-mesh component instead of its own actor, which is what
	// keeps the render thread from drowning in thousands of movable primitives.
	// Only valid inside GenerateChunkAt; the components are owned by the holder.
	AActor* CurrentHolder = nullptr;
	TMap<UStaticMesh*, UInstancedStaticMeshComponent*> CurrentBatches;

	// Per-cell tile data (x, y, layer)->tile, for grid-based auto navigation.
	TMap<FIntVector, int32> WorldTiles;
	TArray<FVector> AutoWaypoints;

	float ShakeTime = 0.f;
	float BobPhase = 0.f;

	// Control / input.
	float InputF = 0.f, InputR = 0.f;
	float ControlYaw = 0.f, ControlPitch = 0.f;
	void OnMoveForward(float V) { InputF = V; }
	void OnMoveRight(float V) { InputR = V; }
	void OnMoveUp(float) {}
	void OnTurn(float V) { ControlYaw += V * LookSensitivity; }
	void OnLookUp(float V) { ControlPitch = FMath::Clamp(ControlPitch + V * LookSensitivity, -89.f, 89.f); }
	void OnToggleMode();

	// Generation.
	FIntVector PlayerChunkCoord() const;
	void UpdateChunksAroundPlayer();
	void GenerateChunkAt(int32 CX, int32 CY, int32 Layer);
	void UnloadChunk(const FIntVector& Key);
	bool HasUpStair(int32 CX, int32 CY, int32 Layer) const;
	bool SolveChunk(FRandomStream& Stream, bool bForceStairCells, TArray<int32>& OutTiles) const;
	bool Propagate(TArray<uint16>& Domains) const;
	bool OpeningsConnected(const TArray<int32>& Tiles, bool bNeedStair) const;

	void SpawnPiece(UStaticMesh* Mesh, const FVector& Loc, const FVector& Scale, UMaterialInterface* Material = nullptr);
	void SpawnLight(const FVector& Loc);

	// Auto-explore: BFS a route from the current cell to a chunk exit opening, follow it.
	void AutoStep(float DeltaSeconds, float& OutMovedDist);
	void BuildAutoPath(const FIntVector& StartCell);
};
