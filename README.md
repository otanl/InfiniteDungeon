# Infinite Dungeon

An endless, procedurally generated dungeon for Unreal Engine 5.8 that you can walk
through — corridors, rooms, doorways, pillars and stairways between floors, all
built at runtime around the player and torn down again once you leave.

The whole thing is one C++ pawn (`AInfiniteDungeonPawn`). Drop it into an empty
level, press Play, and it generates the world around itself.

![Cresting a stairway between floors, walking on through rooms and doorways into a pillared hall](docs/walkthrough.gif)

## Requirements

- **Unreal Engine 5.8**
- **DirectX 12 / Shader Model 6**
- **Hardware ray tracing** — required by MegaLights, which is what makes a hundred
  shadow-casting point lights affordable. See [Performance](#performance).

## Running it

1. Right-click `InfiniteDungeon.uproject` → *Generate Visual Studio project files*
2. Build the `InfiniteDungeonEditor` target (or open the `.sln` and build)
3. Open the project and press **Play**

### Controls

| Key | Action |
| --- | --- |
| `Tab` | Toggle between auto-explore and manual walking |
| `W` `A` `S` `D` | Move (manual mode) |
| Mouse | Look (manual mode) |

In **auto mode** the camera explores on its own: it plans a route through the
current chunk to one of its exits and follows it, occasionally taking a stairway
to another floor. In **manual mode** you walk it yourself, with wall collision and
floor following, so stairs are climbed naturally.

## How it works

**Generation.** The world is a 2D grid of chunks, each solved with a *weighted*
Wave Function Collapse over 16 edge-socket tiles. The weights favour straights and
corners, forbid dead ends and the 4-way cross, and make T-junctions rare — which is
what makes the result read as clean one-cell-wide corridors instead of blobby
half-open space. The middle cell of all four chunk borders is forced open, so
neighbouring chunks always connect, and every chunk is seeded from its coordinates,
so the same place always generates the same way.

**Rooms** are carved on top of the solved tiles, and their boundaries are sealed
except where a corridor genuinely opens into them — that opening becomes a doorway
with jambs and a header. Some rooms get a grid of pillars, placed on cell *corners*
so they never stand in the middle of a walking route.

**Floors** are stacked along Z. Roughly one chunk in `StairRarity` carries a
stairway up to the next layer, built from real-sized steps. Both walking modes
handle the height change through a downward trace that follows the floor.

**Streaming.** Chunks within `LoadRadius` of the player are kept alive and distant
ones destroyed. Building is spread over frames (`ChunksPerTick`) so arriving on a
new floor does not construct the whole neighbourhood in one hitch.

**Rendering.** All geometry in a chunk is batched into a handful of
`UInstancedStaticMeshComponent`s rather than thousands of individual actors.

## Tuning

Everything is `EditAnywhere` — select the pawn and change values in the Details
panel; no recompile needed.

| Property | What it does |
| --- | --- |
| `CellSize` | Corridor width / grid pitch |
| `ChunkCols` `ChunkRows` | Chunk size in cells |
| `WallHeight` `CameraHeight` | Room height, eye height |
| `RoomsPerChunk` | How many rooms are carved per chunk |
| `PillarRoomChance` `PillarRadius` | Chance a room gets pillars, and how thick they are |
| `DoorWidth` `DoorHeight` | Doorway opening |
| `LoadRadius` | View distance, in chunks |
| `ChunksPerTick` | Chunks built per frame while streaming |
| `LayerGap` `StairRarity` | Floor spacing, and how often a stairway appears |
| `MoveSpeed` `ManualSpeed` `TurnSpeed` | Auto speed, manual speed, turn rate (deg/sec) |
| `LightEveryCells` `LightIntensity` `LightRadius` `LightColor` | Lighting |
| `BulbMesh` `BulbMaterial` `BulbSize` `BulbCeilingDrop` | The visible light fixture — swap in a panel, a tube, whatever |
| `SurfaceMaterial` | Material for floors, walls and ceilings |
| `WalkBobAmount` `WalkStride` `WalkBobRoll` `Shake*` | Handheld camera feel |

The walk bob is driven by **distance travelled**, not time, so it stays in step
with whatever speed you set.

## Performance

Keeping a hundred-plus lit, shadow-casting rooms alive at once needs three things,
roughly in order of how much they mattered:

1. **MegaLights** (`r.MegaLights.EnableForProject`, already set in
   `Config/DefaultEngine.ini`). A point light casts shadows through six faces, so
   a hundred of them is several hundred shadow views to set up every frame, and
   the render thread drowns in it long before the GPU is busy. MegaLights makes
   that cost roughly independent of light count. This is why hardware ray tracing
   is a requirement.
2. **Instanced static meshes.** Roughly 190 actors per chunk became a handful of
   components, which is what removed the hitch when crossing between floors.
3. **Spreading chunk builds over frames**, so entering a new layer does not build
   the whole neighbourhood at once.

If you raise `LoadRadius`, raise `LightEveryCells` with it — light count, not
geometry, is what costs.

## Materials

`M_DungeonWhite` and `M_Bulb` are deliberately trivial (a constant colour into base
colour / emissive). If you replace them, make sure **Used with Instanced Static
Meshes** is ticked in the material's Usage settings — without it the surfaces fall
back to the default grey material.

## License

MIT — see [LICENSE](LICENSE).
