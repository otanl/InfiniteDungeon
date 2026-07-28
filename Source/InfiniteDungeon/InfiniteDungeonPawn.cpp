#include "InfiniteDungeonPawn.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InputComponent.h"
#include "Components/SphereComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/PointLight.h"
#include "Engine/HitResult.h"
#include "Materials/MaterialInterface.h"
#include "Engine/World.h"

static constexpr int32 BN = 1;
static constexpr int32 BE = 2;
static constexpr int32 BS = 4;
static constexpr int32 BW = 8;
static constexpr int32 RoomTile = BN | BE | BS | BW;

// Stairwell footprint (interior cells): approach, base, top of a +X flight.
static constexpr int32 SAx = 1, SAy = 1;
static constexpr int32 SBx = 2, SBy = 1;
static constexpr int32 STx = 3, STy = 1;

static const int32 TileWeight[16] = { 4, 0, 0, 5, 0, 7, 5, 1, 0, 5, 7, 1, 5, 1, 1, 0 };

static int32 PopCount16(uint16 M) { int32 N = 0; while (M) { M &= (M - 1); ++N; } return N; }
static int32 ChunkSeed(int32 CX, int32 CY, int32 Salt) { return (CX * 73856093) ^ (CY * 19349663) ^ (Salt * 83492791); }

AInfiniteDungeonPawn::AInfiniteDungeonPawn()
{
	PrimaryActorTick.bCanEverTick = true;

	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	Collision->InitSphereRadius(40.f);
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Collision->SetCollisionObjectType(ECC_Pawn);
	Collision->SetCollisionResponseToAllChannels(ECR_Block);
	RootComponent = Collision;

	Cam = CreateDefaultSubobject<UCameraComponent>(TEXT("Cam"));
	Cam->SetupAttachment(Collision);

	AutoPossessPlayer = EAutoReceiveInput::Player0;
	OverrideInputComponentClass = UInputComponent::StaticClass();
}

void AInfiniteDungeonPawn::BeginPlay()
{
	Super::BeginPlay();

	FloorMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	WallMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	PillarMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (!SurfaceMaterial) { SurfaceMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_DungeonWhite.M_DungeonWhite")); }
	if (!BulbMesh) { BulbMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere")); }
	if (!BulbMaterial) { BulbMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Bulb.M_Bulb")); }

	if (Collision) { Collision->SetSphereRadius(FMath::Max(CollisionRadius, 1.f)); }

	const int32 MidRow = ChunkRows / 2;
	SetActorLocation(FVector(0.f, MidRow * CellSize, CameraHeight));
	ControlYaw = 0.f;
	ControlPitch = 0.f;
	SetActorRotation(FRotator::ZeroRotator);

	LastPlayerChunk = FIntVector(MAX_int32, MAX_int32, MAX_int32);
	UpdateChunksAroundPlayer();
}

bool AInfiniteDungeonPawn::HasUpStair(int32 CX, int32 CY, int32 Layer) const
{
	if (StairRarity <= 0 || ChunkCols < 5 || ChunkRows < 3) { return false; }
	auto Raw = [&](int32 Lyr)
	{
		uint32 H = (uint32)ChunkSeed(CX, CY, Lyr * 2 + 11);
		H ^= H >> 13; H *= 2654435761u; H ^= H >> 15;
		return (H % (uint32)StairRarity) == 0;
	};
	// Never stack a flight directly above another: both would claim the same two
	// cells and fight over the same well.
	return Raw(Layer) && !Raw(Layer - 1);
}

FIntVector AInfiniteDungeonPawn::PlayerChunkCoord() const
{
	const float CW = ChunkCols * CellSize;
	const float CH = ChunkRows * CellSize;
	const FVector P = GetActorLocation();
	const int32 L = FMath::RoundToInt(P.Z / FMath::Max(LayerGap, 1.f));
	return FIntVector(FMath::FloorToInt(P.X / CW), FMath::FloorToInt(P.Y / CH), L);
}

void AInfiniteDungeonPawn::UpdateChunksAroundPlayer()
{
	const FIntVector PC = PlayerChunkCoord();
	const int32 Budget = FMath::Max(ChunksPerTick, 1);

	if (PC != LastPlayerChunk || LoadedChunks.Num() == 0)
	{
		LastPlayerChunk = PC;

		// The chunk under our feet must exist right now - routing reads its tiles
		// and the floor trace needs something to stand on.
		if (!LoadedChunks.Contains(PC)) { GenerateChunkAt(PC.X, PC.Y, PC.Z); }

		// The rest is queued and built a few per frame: arriving on a new layer
		// used to construct the whole 5x5 neighbourhood in a single frame.
		PendingChunks.Reset();
		for (int32 DX = -LoadRadius; DX <= LoadRadius; ++DX)
		{
			for (int32 DY = -LoadRadius; DY <= LoadRadius; ++DY)
			{
				const int32 CX = PC.X + DX, CY = PC.Y + DY;
				auto Want = [&](int32 Lyr)
				{
					const FIntVector K(CX, CY, Lyr);
					if (!LoadedChunks.Contains(K)) { PendingChunks.Add(K); }
				};
				Want(PC.Z);
				// Adjacent layers only where a stair connects, so they can be seen/walked.
				if (HasUpStair(CX, CY, PC.Z)) { Want(PC.Z + 1); }
				if (HasUpStair(CX, CY, PC.Z - 1)) { Want(PC.Z - 1); }
			}
		}
		PendingChunks.Sort([PC](const FIntVector& A, const FIntVector& B)
		{
			const int32 DA = FMath::Abs(A.X - PC.X) + FMath::Abs(A.Y - PC.Y) + FMath::Abs(A.Z - PC.Z) * 4;
			const int32 DB = FMath::Abs(B.X - PC.X) + FMath::Abs(B.Y - PC.Y) + FMath::Abs(B.Z - PC.Z) * 4;
			return DA < DB;
		});
	}

	for (int32 Built = 0; Built < Budget && PendingChunks.Num() > 0; )
	{
		const FIntVector K = PendingChunks[0];
		PendingChunks.RemoveAt(0);
		if (!LoadedChunks.Contains(K)) { GenerateChunkAt(K.X, K.Y, K.Z); ++Built; }
	}

	// Retire distant chunks on the same budget - tearing down a whole strip at
	// once is its own hitch.
	TArray<FIntVector> ToRemove;
	for (const auto& Pair : LoadedChunks)
	{
		if (FMath::Abs(Pair.Key.X - PC.X) > LoadRadius + 1 || FMath::Abs(Pair.Key.Y - PC.Y) > LoadRadius + 1 || FMath::Abs(Pair.Key.Z - PC.Z) > 1)
		{
			ToRemove.Add(Pair.Key);
			if (ToRemove.Num() >= Budget) { break; }
		}
	}
	for (const FIntVector& K : ToRemove) { UnloadChunk(K); }
}

void AInfiniteDungeonPawn::UnloadChunk(const FIntVector& Key)
{
	if (TArray<TObjectPtr<AActor>>* List = LoadedChunks.Find(Key))
	{
		for (const TObjectPtr<AActor>& A : *List) { if (A) { A->Destroy(); } }
	}
	LoadedChunks.Remove(Key);

	const int32 OX = Key.X * ChunkCols, OY = Key.Y * ChunkRows;
	for (int32 X = 0; X < ChunkCols; ++X)
		for (int32 Y = 0; Y < ChunkRows; ++Y)
			WorldTiles.Remove(FIntVector(OX + X, OY + Y, Key.Z));
}

void AInfiniteDungeonPawn::SpawnPiece(UStaticMesh* Mesh, const FVector& Loc, const FVector& Scale, UMaterialInterface* Material)
{
	if (!Mesh || !CurrentHolder) { return; }

	// One component per mesh, shared by every piece in this chunk. Spawning an
	// actor apiece was costing ~190 SpawnActor calls per chunk and left thousands
	// of movable primitives for the render thread to walk every frame.
	UInstancedStaticMeshComponent** Found = CurrentBatches.Find(Mesh);
	UInstancedStaticMeshComponent* Ism = Found ? *Found : nullptr;
	if (!Ism)
	{
		Ism = NewObject<UInstancedStaticMeshComponent>(CurrentHolder);
		Ism->SetStaticMesh(Mesh);
		Ism->SetMaterial(0, Material ? Material : SurfaceMaterial);
		Ism->SetMobility(EComponentMobility::Movable);
		Ism->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Ism->SetCollisionObjectType(ECC_WorldStatic);
		Ism->SetCollisionResponseToAllChannels(ECR_Block);
		Ism->SetupAttachment(CurrentHolder->GetRootComponent());
		Ism->RegisterComponent();
		CurrentBatches.Add(Mesh, Ism);
	}
	Ism->AddInstance(FTransform(FRotator::ZeroRotator, Loc, Scale), true);
}

void AInfiniteDungeonPawn::SpawnLight(const FVector& Loc)
{
	if (!GetWorld()) { return; }
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const float Drop = (BulbCeilingDrop >= 0.f) ? BulbCeilingDrop : FMath::Max(BulbSize * 50.f, 10.f);
	const FVector Pos(Loc.X, Loc.Y, Loc.Z + WallHeight - Drop);
	APointLight* Light = GetWorld()->SpawnActor<APointLight>(Pos, FRotator::ZeroRotator, Params);
	if (!Light) { return; }
	if (UPointLightComponent* PLC = Cast<UPointLightComponent>(Light->GetLightComponent()))
	{
		PLC->SetMobility(EComponentMobility::Movable);
		PLC->SetIntensity(LightIntensity);
		PLC->SetAttenuationRadius(LightRadius);
		PLC->SetLightColor(LightColor);
	}
	if (CurrentChunkList) { CurrentChunkList->Add(Light); }
	if (BulbMesh && BulbSize > 0.f)
	{
		AStaticMeshActor* Bulb = GetWorld()->SpawnActor<AStaticMeshActor>(Pos, FRotator::ZeroRotator, Params);
		if (Bulb)
		{
			Bulb->SetMobility(EComponentMobility::Movable);
			if (UStaticMeshComponent* Comp = Bulb->GetStaticMeshComponent())
			{
				Comp->SetStaticMesh(BulbMesh);
				Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				if (BulbMaterial) { Comp->SetMaterial(0, BulbMaterial); }
			}
			Bulb->SetActorScale3D(FVector(BulbSize));
			if (CurrentChunkList) { CurrentChunkList->Add(Bulb); }
		}
	}
}

bool AInfiniteDungeonPawn::Propagate(TArray<uint16>& Domains) const
{
	const int32 Cols = ChunkCols, Rows = ChunkRows;
	auto Idx = [Rows](int32 X, int32 Y) { return X * Rows + Y; };
	static const int32 Dirs[4][4] = { { 1, 0, BE, BW }, { -1, 0, BW, BE }, { 0, 1, BN, BS }, { 0, -1, BS, BN } };
	TArray<int32> Queue; Queue.Reserve(Cols * Rows * 2);
	for (int32 I = 0; I < Cols * Rows; ++I) { Queue.Add(I); }
	int32 QHead = 0;
	while (QHead < Queue.Num())
	{
		const int32 C = Queue[QHead++]; const int32 X = C / Rows, Y = C % Rows; const uint16 Dc = Domains[C];
		for (int32 D = 0; D < 4; ++D)
		{
			const int32 NX = X + Dirs[D][0], NY = Y + Dirs[D][1];
			if (NX < 0 || NY < 0 || NX >= Cols || NY >= Rows) { continue; }
			const int32 ABit = Dirs[D][2], BBit = Dirs[D][3];
			bool AOpen = false, AClosed = false;
			for (int32 T = 0; T < 16; ++T) { if (Dc & (1 << T)) { if (T & ABit) AOpen = true; else AClosed = true; } }
			const int32 NC = Idx(NX, NY); const uint16 Before = Domains[NC]; uint16 After = 0;
			for (int32 T = 0; T < 16; ++T)
			{
				if (!(Before & (1 << T))) { continue; }
				const bool WantOpen = (T & BBit) != 0;
				if ((WantOpen && AOpen) || (!WantOpen && AClosed)) { After |= (uint16)(1 << T); }
			}
			if (After == 0) { return false; }
			if (After != Before) { Domains[NC] = After; Queue.Add(NC); }
		}
	}
	return true;
}

bool AInfiniteDungeonPawn::SolveChunk(FRandomStream& Stream, bool bForceStairCells, TArray<int32>& OutTiles) const
{
	const int32 Cols = ChunkCols, Rows = ChunkRows;
	const int32 MidRow = Rows / 2, MidCol = Cols / 2;
	auto Idx = [Rows](int32 X, int32 Y) { return X * Rows + Y; };

	uint16 AllowedMask = 0;
	for (int32 T = 0; T < 16; ++T) { if (TileWeight[T] > 0) { AllowedMask |= (uint16)(1 << T); } }

	TArray<uint16> Domains; Domains.Init(AllowedMask, Cols * Rows);
	for (int32 X = 0; X < Cols; ++X)
	{
		for (int32 Y = 0; Y < Rows; ++Y)
		{
			uint16 Mask = AllowedMask;
			for (int32 T = 0; T < 16; ++T)
			{
				bool Remove = false;
				if (X == 0) { if (Y == MidRow) { if (!(T & BW)) Remove = true; } else if (T & BW) Remove = true; }
				if (X == Cols - 1) { if (Y == MidRow) { if (!(T & BE)) Remove = true; } else if (T & BE) Remove = true; }
				if (Y == 0) { if (X == MidCol) { if (!(T & BS)) Remove = true; } else if (T & BS) Remove = true; }
				if (Y == Rows - 1) { if (X == MidCol) { if (!(T & BN)) Remove = true; } else if (T & BN) Remove = true; }
				if (Remove) { Mask &= ~(uint16)(1 << T); }
			}
			Domains[Idx(X, Y)] = Mask;
		}
	}

	if (bForceStairCells)
	{
		// Force the stair strip open and horizontally linked (SA-SB-ST along +X).
		auto KeepIf = [&](int32 X, int32 Y, int32 NeedBits)
		{
			uint16 M = Domains[Idx(X, Y)];
			for (int32 T = 0; T < 16; ++T) { if ((M & (1 << T)) && ((T & NeedBits) != NeedBits)) { M &= ~(uint16)(1 << T); } }
			Domains[Idx(X, Y)] = M;
		};
		KeepIf(SAx, SAy, BE);           // approach opens east to base
		KeepIf(SBx, SBy, BE | BW);      // base links both ways
		KeepIf(STx, STy, BW | BE);      // top links back to base and out to the landing
		KeepIf(STx + 1, STy, BW);       // landing: where you step off at the top
	}

	if (!Propagate(Domains)) { return false; }

	while (true)
	{
		int32 Best = -1, BestCount = 99;
		for (int32 I = 0; I < Cols * Rows; ++I)
		{
			const int32 Cnt = PopCount16(Domains[I]);
			if (Cnt == 0) { return false; }
			if (Cnt > 1 && Cnt < BestCount) { BestCount = Cnt; Best = I; }
		}
		if (Best < 0) { break; }
		int32 Sum = 0;
		for (int32 T = 0; T < 16; ++T) { if (Domains[Best] & (1 << T)) { Sum += TileWeight[T]; } }
		int32 Roll = Stream.RandRange(0, FMath::Max(Sum - 1, 0)); int32 Pick = -1;
		for (int32 T = 0; T < 16; ++T) { if (Domains[Best] & (1 << T)) { Roll -= TileWeight[T]; if (Roll < 0) { Pick = T; break; } } }
		if (Pick < 0) { return false; }
		Domains[Best] = (uint16)(1 << Pick);
		if (!Propagate(Domains)) { return false; }
	}

	OutTiles.SetNum(Cols * Rows);
	for (int32 I = 0; I < Cols * Rows; ++I)
	{
		int32 Value = 0;
		for (int32 T = 0; T < 16; ++T) { if (Domains[I] & (1 << T)) { Value = T; break; } }
		OutTiles[I] = Value;
	}
	return true;
}

bool AInfiniteDungeonPawn::OpeningsConnected(const TArray<int32>& Tiles, bool bNeedStair) const
{
	const int32 Cols = ChunkCols, Rows = ChunkRows;
	const int32 MidRow = Rows / 2, MidCol = Cols / 2;
	auto Idx = [Rows](int32 X, int32 Y) { return X * Rows + Y; };
	const int32 West = Idx(0, MidRow), East = Idx(Cols - 1, MidRow), South = Idx(MidCol, 0), North = Idx(MidCol, Rows - 1);

	TArray<int32> Prev; Prev.Init(-1, Cols * Rows);
	TArray<int32> Q; Q.Add(West); Prev[West] = West;
	int32 Head = 0;
	while (Head < Q.Num())
	{
		const int32 C = Q[Head++]; const int32 X = C / Rows, Y = C % Rows, T = Tiles[C];
		const int32 Steps[4][3] = { { 1, 0, BE }, { -1, 0, BW }, { 0, 1, BN }, { 0, -1, BS } };
		for (int32 D = 0; D < 4; ++D)
		{
			if (!(T & Steps[D][2])) { continue; }
			const int32 NX = X + Steps[D][0], NY = Y + Steps[D][1];
			if (NX < 0 || NY < 0 || NX >= Cols || NY >= Rows) { continue; }
			// Routing never crosses the stair strip, so validate the chunk without
			// it: it has to work as a flat maze on its own, and both ends of the
			// flight have to be reachable that way.
			if (bNeedStair && ((NX == SBx && NY == SBy) || (NX == STx && NY == STy))) { continue; }
			const int32 NC = Idx(NX, NY);
			if (Tiles[NC] != 0 && Prev[NC] == -1) { Prev[NC] = C; Q.Add(NC); }
		}
	}
	if (Prev[East] == -1 || Prev[South] == -1 || Prev[North] == -1) { return false; }
	if (bNeedStair && (Prev[Idx(SAx, SAy)] == -1 || Prev[Idx(STx + 1, STy)] == -1)) { return false; }
	return true;
}

void AInfiniteDungeonPawn::GenerateChunkAt(int32 CX, int32 CY, int32 Layer)
{
	const FIntVector Key(CX, CY, Layer);
	if (LoadedChunks.Contains(Key)) { return; }
	LoadedChunks.Add(Key, TArray<TObjectPtr<AActor>>());
	CurrentChunkList = LoadedChunks.Find(Key);

	// One holder actor per chunk owns this chunk's instanced-mesh components, so
	// unloading the chunk tears all of its geometry down with it.
	CurrentBatches.Reset();
	CurrentHolder = nullptr;
	if (GetWorld())
	{
		FActorSpawnParameters HolderParams;
		HolderParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		CurrentHolder = GetWorld()->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, HolderParams);
		if (CurrentHolder)
		{
			USceneComponent* HolderRoot = NewObject<USceneComponent>(CurrentHolder);
			CurrentHolder->SetRootComponent(HolderRoot);
			HolderRoot->RegisterComponent();
			CurrentChunkList->Add(CurrentHolder);
		}
	}

	const int32 Cols = ChunkCols, Rows = ChunkRows;
	const int32 MidRow = Rows / 2, MidCol = Cols / 2;
	auto Idx = [Rows](int32 X, int32 Y) { return X * Rows + Y; };

	const bool bUp = HasUpStair(CX, CY, Layer);
	const bool bDown = HasUpStair(CX, CY, Layer - 1);
	const bool bStair = bUp || bDown;

	TArray<int32> Tiles;
	bool bOk = false;
	for (int32 Attempt = 0; Attempt < 50 && !bOk; ++Attempt)
	{
		FRandomStream Stream(ChunkSeed(CX, CY, Attempt * 31 + Layer * 7));
		if (SolveChunk(Stream, bStair, Tiles) && OpeningsConnected(Tiles, bStair)) { bOk = true; }
	}
	if (!bOk)
	{
		Tiles.Init(0, Cols * Rows);
		for (int32 X = 0; X < Cols; ++X) { Tiles[Idx(X, MidRow)] |= (BE | BW); }
		for (int32 Y = 0; Y < Rows; ++Y) { Tiles[Idx(MidCol, Y)] |= (BN | BS); }
		if (bStair)
		{
			// Approach, flight and landing, hung off the central corridor.
			for (int32 X = SAx; X <= STx + 1; ++X) { Tiles[Idx(X, SAy)] |= (BE | BW); }
			Tiles[Idx(MidCol, SAy)] |= (BN | BS);
		}
	}

	// Rooms.
	struct FRoomRect { int32 CX, CY, HW, HH; bool bPillars; };
	TArray<FRoomRect> Rooms;
	{
		FRandomStream RS(ChunkSeed(CX, CY, 777 + Layer * 3));
		for (int32 R = 0; R < RoomsPerChunk; ++R)
		{
			FRoomRect Room;
			Room.CX = RS.RandRange(2, Cols - 3); Room.CY = RS.RandRange(2, Rows - 3);
			Room.HW = RS.RandRange(1, 2); Room.HH = RS.RandRange(1, 2);
			Room.bPillars = (RS.FRand() < PillarRoomChance);
			for (int32 XX = Room.CX - Room.HW; XX <= Room.CX + Room.HW; ++XX)
				for (int32 YY = Room.CY - Room.HH; YY <= Room.CY + Room.HH; ++YY)
					if (XX >= 1 && XX <= Cols - 2 && YY >= 1 && YY <= Rows - 2) { Tiles[Idx(XX, YY)] = RoomTile; }
			Rooms.Add(Room);
		}
	}

	const int32 OX = CX * Cols, OY = CY * Rows;
	const float ZB = Layer * LayerGap;

	// Record tiles for grid-based auto navigation.
	for (int32 X = 0; X < Cols; ++X)
		for (int32 Y = 0; Y < Rows; ++Y)
			WorldTiles.Add(FIntVector(OX + X, OY + Y, Layer), Tiles[Idx(X, Y)]);
	const float WallLen = (CellSize + 30.f) / 100.f;
	const float WallThick = 0.3f;
	const FVector FloorScale(CellSize / 100.f, CellSize / 100.f, 1.f);
	const FVector WallY(WallThick, WallLen, WallHeight / 100.f);
	const FVector WallX(WallLen, WallThick, WallHeight / 100.f);
	const FVector Up(0.f, 0.f, WallHeight * 0.5f);

	const int32 IdxSB = Idx(SBx, SBy), IdxST = Idx(STx, STy);
	auto IsStairCell = [&](int32 X, int32 Y) { return (X == SBx && Y == SBy) || (X == STx && Y == STy); };

	// Floors, ceilings, lights.
	for (int32 X = 0; X < Cols; ++X)
	{
		for (int32 Y = 0; Y < Rows; ++Y)
		{
			if (Tiles[Idx(X, Y)] == 0) { continue; }
			const FVector Center((OX + X) * CellSize, (OY + Y) * CellSize, ZB);

			const bool bStairFloor = bUp && IsStairCell(X, Y);      // stair replaces flat floor
			const bool bHole = bDown && !bUp && IsStairCell(X, Y);  // lower stair provides the surface here
			if (!bStairFloor && !bHole) { SpawnPiece(FloorMesh, Center, FloorScale); }

			if (bCeiling && !(bUp && IsStairCell(X, Y)))
			{
				SpawnPiece(WallMesh, Center + FVector(0.f, 0.f, WallHeight), FVector(CellSize / 100.f, CellSize / 100.f, 0.3f));
			}
			if (LightEveryCells > 0 && ((OX + X) % LightEveryCells == 0) && ((OY + Y) % LightEveryCells == 0) && !IsStairCell(X, Y))
			{
				SpawnLight(FVector((OX + X) * CellSize, (OY + Y) * CellSize, ZB));
			}
		}
	}

	// Walls (interior E/N; west/south chunk borders sealed except the mid opening).
	for (int32 X = 0; X < Cols; ++X)
	{
		for (int32 Y = 0; Y < Rows; ++Y)
		{
			const int32 T = Tiles[Idx(X, Y)];
			const FVector Center((OX + X) * CellSize, (OY + Y) * CellSize, ZB);
			if (X + 1 < Cols)
			{
				const int32 NT = Tiles[Idx(X + 1, Y)]; const bool AO = T != 0, BO = NT != 0; bool bWall = false;
				if (AO || BO)
				{
					if (AO != BO) bWall = true;
					else if ((T == RoomTile) != (NT == RoomTile)) bWall = false;
					else if (T != RoomTile) bWall = !(T & BE);
				}
				if (bWall) { SpawnPiece(WallMesh, Center + FVector(CellSize * 0.5f, 0, 0) + Up, WallY); }
			}
			if (Y + 1 < Rows)
			{
				const int32 NT = Tiles[Idx(X, Y + 1)]; const bool AO = T != 0, BO = NT != 0; bool bWall = false;
				if (AO || BO)
				{
					if (AO != BO) bWall = true;
					else if ((T == RoomTile) != (NT == RoomTile)) bWall = false;
					else if (T != RoomTile) bWall = !(T & BN);
				}
				if (bWall) { SpawnPiece(WallMesh, Center + FVector(0, CellSize * 0.5f, 0) + Up, WallX); }
			}
			if (X == 0 && Y != MidRow) { SpawnPiece(WallMesh, Center + FVector(-CellSize * 0.5f, 0, 0) + Up, WallY); }
			if (Y == 0 && X != MidCol) { SpawnPiece(WallMesh, Center + FVector(0, -CellSize * 0.5f, 0) + Up, WallX); }
		}
	}

	// Doorways.
	if (bDoorways)
	{
		const float HalfCell = CellSize * 0.5f;
		const float HalfDoor = FMath::Min(DoorWidth * 0.5f, HalfCell - 20.f);
		const float JambW = FMath::Max(HalfCell - HalfDoor, 1.f);
		const float JambCenter = (HalfCell + HalfDoor) * 0.5f;
		const float Thick = 0.3f, FullH = WallHeight / 100.f;
		const float HeaderH = FMath::Max((WallHeight - DoorHeight) / 100.f, 0.1f);
		const float HeaderZ = (WallHeight + DoorHeight) * 0.5f;
		const int32 NB[4][3] = { { 1, 0, BW }, { -1, 0, BE }, { 0, 1, BS }, { 0, -1, BN } };
		for (int32 X = 0; X < Cols; ++X)
		{
			for (int32 Y = 0; Y < Rows; ++Y)
			{
				if (Tiles[Idx(X, Y)] != RoomTile) { continue; }
				for (int32 D = 0; D < 4; ++D)
				{
					const int32 NX = X + NB[D][0], NY = Y + NB[D][1];
					if (NX < 0 || NY < 0 || NX >= Cols || NY >= Rows) { continue; }
					const int32 NT = Tiles[Idx(NX, NY)];
					if (NT == 0 || NT == RoomTile) { continue; }
					const FVector WA((OX + X) * CellSize, (OY + Y) * CellSize, ZB);
					const FVector WB((OX + NX) * CellSize, (OY + NY) * CellSize, ZB);
					const FVector Mid = (WA + WB) * 0.5f;
					const bool bAlongX = (NB[D][0] != 0);
					const bool bDoor = (NT & NB[D][2]) != 0;
					if (!bDoor)
					{
						const FVector SealScale = bAlongX ? FVector(Thick, WallLen, FullH) : FVector(WallLen, Thick, FullH);
						SpawnPiece(WallMesh, Mid + FVector(0, 0, WallHeight * 0.5f), SealScale);
						continue;
					}
					if (bAlongX)
					{
						SpawnPiece(WallMesh, Mid + FVector(0, -JambCenter, WallHeight * 0.5f), FVector(Thick, JambW / 100.f, FullH));
						SpawnPiece(WallMesh, Mid + FVector(0, JambCenter, WallHeight * 0.5f), FVector(Thick, JambW / 100.f, FullH));
						if (DoorHeight < WallHeight) SpawnPiece(WallMesh, Mid + FVector(0, 0, HeaderZ), FVector(Thick, (HalfDoor * 2.f) / 100.f, HeaderH));
					}
					else
					{
						SpawnPiece(WallMesh, Mid + FVector(-JambCenter, 0, WallHeight * 0.5f), FVector(JambW / 100.f, Thick, FullH));
						SpawnPiece(WallMesh, Mid + FVector(JambCenter, 0, WallHeight * 0.5f), FVector(JambW / 100.f, Thick, FullH));
						if (DoorHeight < WallHeight) SpawnPiece(WallMesh, Mid + FVector(0, 0, HeaderZ), FVector((HalfDoor * 2.f) / 100.f, Thick, HeaderH));
					}
				}
			}
		}
	}

	// Pillars.
	if (PillarMesh)
	{
		const FVector PillarScale(PillarRadius, PillarRadius, WallHeight / 100.f);
		for (const FRoomRect& Room : Rooms)
		{
			if (!Room.bPillars) { continue; }
			// Pillars stand on cell corners, never cell centres: the walker follows
			// the centre lines, so a pillar on a centre sits right in the route.
			for (int32 XX = Room.CX - Room.HW; XX < Room.CX + Room.HW; ++XX)
				for (int32 YY = Room.CY - Room.HH; YY < Room.CY + Room.HH; ++YY)
				{
					// Corner shared by (XX,YY)..(XX+1,YY+1) - all four cells must be room floor.
					bool bClear = true;
					for (int32 AX = XX; AX <= XX + 1 && bClear; ++AX)
						for (int32 AY = YY; AY <= YY + 1 && bClear; ++AY)
						{
							if (AX < 1 || AX > Cols - 2 || AY < 1 || AY > Rows - 2) { bClear = false; }
							else if (Tiles[Idx(AX, AY)] != RoomTile || IsStairCell(AX, AY)) { bClear = false; }
						}
					if (!bClear) { continue; }
					SpawnPiece(PillarMesh, FVector((OX + XX + 0.5f) * CellSize, (OY + YY + 0.5f) * CellSize, ZB + WallHeight * 0.5f), PillarScale);
				}
		}
	}

	// Stairway up to the next layer (a straight +X flight over the base and top cells).
	if (bUp)
	{
		const float StartWX = (OX + SBx) * CellSize - CellSize * 0.5f;
		const float RunLen = 2.f * CellSize;
		const float MidYW = (OY + SBy) * CellSize;
		const int32 NumSteps = FMath::Max(FMath::CeilToInt(LayerGap / 18.f), 1);
		const float TreadLen = RunLen / NumSteps;
		const float Riser = LayerGap / NumSteps;
		const float BaseBottom = ZB - 100.f;
		for (int32 I = 0; I < NumSteps; ++I)
		{
			const float TreadTop = ZB + (I + 1) * Riser;
			const float CXW = StartWX + (I + 0.5f) * TreadLen;
			SpawnPiece(WallMesh, FVector(CXW, MidYW, (BaseBottom + TreadTop) * 0.5f),
				FVector(TreadLen / 100.f, CellSize / 100.f, (TreadTop - BaseBottom) / 100.f));
		}
		// Shaft side walls (span both layers so the well is enclosed).
		const float SideTop = ZB + LayerGap + WallHeight;
		const float SideCX = StartWX + RunLen * 0.5f;
		const FVector SideScale((RunLen + 30.f) / 100.f, 0.3f, (SideTop - BaseBottom) / 100.f);
		SpawnPiece(WallMesh, FVector(SideCX, MidYW - CellSize * 0.5f, (BaseBottom + SideTop) * 0.5f), SideScale);
		SpawnPiece(WallMesh, FVector(SideCX, MidYW + CellSize * 0.5f, (BaseBottom + SideTop) * 0.5f), SideScale);
	}

	// The layer above a stairway carries a two-cell well in its floor. The flight
	// arrives at the east end, so wall off the west end: otherwise you look
	// straight at the cut edge of the floor slabs (which are single-sided planes,
	// so the well reads as a hole through to nothing) and can walk into the drop.
	if (bDown && !bUp)
	{
		const float WestX = (OX + SBx) * CellSize - CellSize * 0.5f;
		const float WellY = (OY + SBy) * CellSize;
		// Run it from the lower corridor's ceiling up to ours, so it also covers the
		// dead band between the two layers - stopping at our own floor would leave a
		// slot there looking out of the dungeon. Below that is the stair entrance,
		// which must stay open.
		// Ceiling slabs are 30cm thick centred on WallHeight, so meet the slab's
		// underside: stopping at its centre line leaves a 15cm notch on the well
		// side. The top end is left at the centre so it buries into our own slab.
		const float Bottom = ZB - LayerGap + WallHeight - 15.f;
		const float Top = ZB + WallHeight;
		SpawnPiece(WallMesh, FVector(WestX, WellY, (Bottom + Top) * 0.5f),
			FVector(0.3f, (CellSize + 30.f) / 100.f, (Top - Bottom) / 100.f));
	}

	CurrentChunkList = nullptr;
	CurrentHolder = nullptr;
	CurrentBatches.Reset();
}

void AInfiniteDungeonPawn::BuildAutoPath(const FIntVector& Start)
{
	AutoWaypoints.Reset();
	const int32 Cols = ChunkCols, Rows = ChunkRows, L = Start.Z;
	const int32 MidR = Rows / 2, MidC = Cols / 2;
	const int32 CcX = FMath::FloorToInt((float)Start.X / (float)Cols);
	const int32 CcY = FMath::FloorToInt((float)Start.Y / (float)Rows);
	const int32 OX = CcX * Cols, OY = CcY * Rows;

	const int32 SLx = Start.X - OX, SLy = Start.Y - OY;
	if (SLx < 0 || SLy < 0 || SLx >= Cols || SLy >= Rows) { return; }
	auto LIdx = [Rows](int32 LX, int32 LY) { return LX * Rows + LY; };

	// Stair cells hold the flight itself, or the well opening from the layer
	// below - no flat floor either way. Routing over one would climb a layer or
	// drop into the well, leaving the walker off its planned route. Manual
	// walking can still use them.
	const bool bStairChunk = HasUpStair(CcX, CcY, L) || HasUpStair(CcX, CcY, L - 1);
	auto Blocked = [&](int32 LX, int32 LY)
	{
		return bStairChunk && ((LX == SBx && LY == SBy) || (LX == STx && LY == STy));
	};

	// BFS from the start cell across this chunk's open cells.
	TArray<int32> Prev; Prev.Init(-1, Cols * Rows);
	TArray<int32> Q; Q.Add(LIdx(SLx, SLy)); Prev[LIdx(SLx, SLy)] = LIdx(SLx, SLy);
	// A step needs BOTH cells open toward each other. Rooms are carved over the
	// WFC tiles as fully open, so a room cell on its own would claim passage
	// through a corridor edge that is actually walled off.
	const int32 Steps[4][4] = { { 1, 0, BE, BW }, { -1, 0, BW, BE }, { 0, 1, BN, BS }, { 0, -1, BS, BN } };
	int32 Head = 0;
	while (Head < Q.Num())
	{
		const int32 C = Q[Head++]; const int32 LX = C / Rows, LY = C % Rows;
		const int32 GT = WorldTiles.FindRef(FIntVector(OX + LX, OY + LY, L));
		for (int32 D = 0; D < 4; ++D)
		{
			if (!(GT & Steps[D][2])) { continue; }
			const int32 NLx = LX + Steps[D][0], NLy = LY + Steps[D][1];
			if (NLx < 0 || NLy < 0 || NLx >= Cols || NLy >= Rows) { continue; }
			if (Blocked(NLx, NLy)) { continue; }
			const int32* NT = WorldTiles.Find(FIntVector(OX + NLx, OY + NLy, L));
			if (!NT || *NT == 0 || !(*NT & Steps[D][3])) { continue; }
			if (Prev[LIdx(NLx, NLy)] == -1) { Prev[LIdx(NLx, NLy)] = C; Q.Add(LIdx(NLx, NLy)); }
		}
	}

	// Destinations: the four edge openings, plus the stairway when this chunk has
	// one. Choosing a stair is how auto mode changes layer.
	constexpr int32 GoalExit = 0, GoalUp = 1, GoalDown = 2, GoalStranded = 3;
	struct FGoalOption { int32 Cell; int32 Kind; };
	TArray<FGoalOption> Options;

	const FIntVector Ops[4] = { { 0, MidR, 0 }, { Cols - 1, MidR, 0 }, { MidC, 0, 0 }, { MidC, Rows - 1, 0 } };
	for (int32 O = 0; O < 4; ++O)
	{
		const int32 EC = LIdx(Ops[O].X, Ops[O].Y);
		if (Prev[EC] != -1 && EC != LIdx(SLx, SLy)) { Options.Add({ EC, GoalExit }); }
	}
	// Up: walk to the approach, then east along the flight. Down: walk to the
	// landing, then west down it. Either way the floor-follow carries the height.
	if (HasUpStair(CcX, CcY, L) && Prev[LIdx(SAx, SAy)] != -1) { Options.Add({ LIdx(SAx, SAy), GoalUp }); }
	if (HasUpStair(CcX, CcY, L - 1) && Prev[LIdx(STx + 1, STy)] != -1) { Options.Add({ LIdx(STx + 1, STy), GoalDown }); }
	if (Options.Num() == 0)
	{
		for (int32 O = 0; O < 4; ++O)
		{
			const int32 EC = LIdx(Ops[O].X, Ops[O].Y);
			if (Prev[EC] != -1) { Options.Add({ EC, GoalExit }); }
		}
	}

	// With nothing reachable, walk to the farthest cell BFS found rather than freeze.
	int32 Goal = -1, Kind = GoalStranded;
	if (Options.Num() > 0)
	{
		const FGoalOption& Chosen = Options[FMath::RandRange(0, Options.Num() - 1)];
		Goal = Chosen.Cell;
		Kind = Chosen.Kind;
	}
	else if (Q.Num() > 1) { Goal = Q.Last(); }
	if (Goal < 0) { return; }

	// Reconstruct path start->goal and turn cells into world waypoints.
	TArray<int32> Rev; int32 Cur = Goal;
	while (Cur != LIdx(SLx, SLy)) { Rev.Add(Cur); Cur = Prev[Cur]; }
	for (int32 I = Rev.Num() - 1; I >= 0; --I)
	{
		const int32 LX = Rev[I] / Rows, LY = Rev[I] % Rows;
		AutoWaypoints.Add(FVector((OX + LX) * CellSize, (OY + LY) * CellSize, 0.f));
	}

	auto AddFlight = [&](int32 FromX, int32 ToX, int32 StepX)
	{
		for (int32 X = FromX; X != ToX + StepX; X += StepX)
		{
			AutoWaypoints.Add(FVector((OX + X) * CellSize, (OY + SBy) * CellSize, 0.f));
		}
	};

	if (Kind == GoalExit)
	{
		// One cell past the exit, into the neighbour, so we cross the seam and re-plan there.
		const int32 ELx = Goal / Rows, ELy = Goal % Rows;
		int32 BX = OX + ELx, BY = OY + ELy;
		if (ELx == 0) BX -= 1; else if (ELx == Cols - 1) BX += 1;
		if (ELy == 0) BY -= 1; else if (ELy == Rows - 1) BY += 1;
		AutoWaypoints.Add(FVector(BX * CellSize, BY * CellSize, 0.f));
	}
	else if (Kind == GoalUp) { AddFlight(SBx, STx + 1, 1); }
	else if (Kind == GoalDown) { AddFlight(STx, SAx, -1); }
}

void AInfiniteDungeonPawn::AutoStep(float DeltaSeconds, float& OutMovedDist)
{
	const FVector P = GetActorLocation();
	const int32 L = FMath::RoundToInt(P.Z / FMath::Max(LayerGap, 1.f));
	const FIntVector Cell(FMath::RoundToInt(P.X / CellSize), FMath::RoundToInt(P.Y / CellSize), L);

	if (AutoWaypoints.Num() == 0) { BuildAutoPath(Cell); }
	if (AutoWaypoints.Num() == 0) { return; }

	const float Step = MoveSpeed * DeltaSeconds;
	FVector Tgt = AutoWaypoints[0]; Tgt.Z = P.Z;

	// Walk one axis at a time. The route runs centre-to-centre through open
	// cells, so staying on the centre line keeps every doorway clear; heading
	// diagonally at the next waypoint is what used to wedge us on a jamb.
	FVector Delta = Tgt - P; Delta.Z = 0.f;
	if (FMath::Abs(Delta.X) >= FMath::Abs(Delta.Y)) { Delta.Y = 0.f; } else { Delta.X = 0.f; }
	const float Dist = Delta.Size();

	// Deliberately unswept: the route only crosses open cells, and a blocked
	// sweep would leave the walker stuck with nothing to push it free.
	if (Dist <= Step)
	{
		const FVector Arrived = P + Delta;
		SetActorLocation(FVector(Arrived.X, Arrived.Y, P.Z));
		OutMovedDist = Dist;
		if (FVector::Dist2D(GetActorLocation(), Tgt) < 1.f)
		{
			AutoWaypoints.RemoveAt(0);
			if (AutoWaypoints.Num() == 0) { BuildAutoPath(Cell); }
		}
	}
	else
	{
		SetActorLocation(P + (Delta / Dist) * Step);
		OutMovedDist = Step;
	}

	FVector Face = Tgt - GetActorLocation(); Face.Z = 0.f;
	if (Face.SizeSquared() > 100.f)
	{
		const FRotator NewRot = FMath::RInterpConstantTo(GetActorRotation(), FRotator(0.f, Face.Rotation().Yaw, 0.f), DeltaSeconds, TurnSpeed);
		SetActorRotation(FRotator(0.f, NewRot.Yaw, 0.f));
	}
}

void AInfiniteDungeonPawn::OnToggleMode()
{
	bManualMode = !bManualMode;
	if (bManualMode)
	{
		const FRotator R = GetActorRotation();
		ControlYaw = R.Yaw;
		ControlPitch = R.Pitch;
	}
	else
	{
		// Re-enter auto on the centre line and re-plan, so the walker never starts
		// a leg from an off-centre spot where a corridor would clip it.
		const FVector P = GetActorLocation();
		SetActorLocation(FVector(FMath::RoundToInt(P.X / CellSize) * CellSize,
			FMath::RoundToInt(P.Y / CellSize) * CellSize, P.Z));
		AutoWaypoints.Reset();
		ControlPitch = 0.f;
	}
}

void AInfiniteDungeonPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	if (!PlayerInputComponent) { return; }
	PlayerInputComponent->BindAxis("Dungeon_MoveForward", this, &AInfiniteDungeonPawn::OnMoveForward);
	PlayerInputComponent->BindAxis("Dungeon_MoveRight", this, &AInfiniteDungeonPawn::OnMoveRight);
	PlayerInputComponent->BindAxis("Dungeon_MoveUp", this, &AInfiniteDungeonPawn::OnMoveUp);
	PlayerInputComponent->BindAxis("Dungeon_Turn", this, &AInfiniteDungeonPawn::OnTurn);
	PlayerInputComponent->BindAxis("Dungeon_LookUp", this, &AInfiniteDungeonPawn::OnLookUp);
	PlayerInputComponent->BindAction("Dungeon_ToggleMode", IE_Pressed, this, &AInfiniteDungeonPawn::OnToggleMode);
}

void AInfiniteDungeonPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UpdateChunksAroundPlayer();

	float MovedDist = 0.f;
	if (bManualMode)
	{
		SetActorRotation(FRotator(ControlPitch, ControlYaw, 0.f));
		const FVector Fwd = FRotator(0.f, ControlYaw, 0.f).Vector();
		const FVector Right = FRotator(0.f, ControlYaw + 90.f, 0.f).Vector();
		const FVector Move = Fwd * InputF + Right * InputR;
		const FVector Before = GetActorLocation();
		if (!Move.IsNearlyZero()) { AddActorWorldOffset(Move.GetSafeNormal() * (ManualSpeed * DeltaSeconds), true); }
		MovedDist = FVector::Dist2D(Before, GetActorLocation());
	}
	else
	{
		AutoStep(DeltaSeconds, MovedDist);
	}

	// Follow the floor below (both modes) so stairs are walked and Z stays at eye height.
	if (GetWorld())
	{
		const FVector P = GetActorLocation();
		FHitResult Hit; FCollisionQueryParams QP; QP.AddIgnoredActor(this);
		if (GetWorld()->LineTraceSingleByChannel(Hit, P + FVector(0, 0, 80.f), P - FVector(0, 0, 4000.f), ECC_Visibility, QP))
		{
			// Ignore a floor found far below: that is a stairwell opening, not the
			// ground under our feet, and following it would drop us a whole layer.
			// Real risers are ~18cm, so this never interferes with walking stairs.
			const float TargetZ = Hit.ImpactPoint.Z + CameraHeight;
			if (TargetZ > P.Z - 150.f) { SetActorLocation(FVector(P.X, P.Y, TargetZ)); }
		}
	}

	if (Cam)
	{
		if (bCameraShake)
		{
			ShakeTime += DeltaSeconds * ShakeSpeed;
			BobPhase += (MovedDist / FMath::Max(WalkStride, 1.f)) * 2.f * PI;
			const float T = ShakeTime, B = BobPhase;
			const float VBob = FMath::Sin(B * 2.f) * WalkBobAmount;
			const float HBob = FMath::Sin(B) * (WalkBobAmount * 0.55f);
			const float RollBob = FMath::Sin(B) * WalkBobRoll;
			const float PitchBob = FMath::Cos(B * 2.f) * (WalkBobRoll * 0.4f);
			const float DriftY = FMath::Sin(T * 1.3f) * ShakePosAmount;
			const float DriftZ = FMath::Sin(T * 1.7f + 1.f) * ShakePosAmount * 0.5f;
			const float DriftYaw = FMath::Sin(T * 1.1f) * ShakeRotAmount;
			const float DriftPitch = FMath::Sin(T * 0.9f + 0.5f) * ShakeRotAmount;
			const float JitYaw = FMath::Sin(T * 24.f) * ShakeJitter;
			const float JitPitch = FMath::Sin(T * 19.f) * ShakeJitter;
			Cam->SetRelativeLocation(FVector(0.f, HBob + DriftY, VBob + DriftZ));
			Cam->SetRelativeRotation(FRotator(PitchBob + DriftPitch + JitPitch, DriftYaw + JitYaw, RollBob));
		}
		else { Cam->SetRelativeLocationAndRotation(FVector::ZeroVector, FRotator::ZeroRotator); }
	}
}
