#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

using Gdiplus::Bitmap;
using Gdiplus::Color;
using Gdiplus::ColorMatrix;
using Gdiplus::Font;
using Gdiplus::FontFamily;
using Gdiplus::Graphics;
using Gdiplus::Image;
using Gdiplus::ImageAttributes;
using Gdiplus::LinearGradientBrush;
using Gdiplus::Pen;
using Gdiplus::PointF;
using Gdiplus::Rect;
using Gdiplus::RectF;
using Gdiplus::SolidBrush;
using Gdiplus::Status;

namespace {

constexpr int kSpriteW = 32;
constexpr int kSpriteH = 32;
constexpr int kFootprintW = 32;
constexpr int kFootprintH = 16;
constexpr int kHalfW = kFootprintW / 2;
constexpr int kHalfH = kFootprintH / 2;
constexpr int kWorldSize = 24;
constexpr int kPlayers = 2;
constexpr int kSpheresPerPlayer = 3;
constexpr int kArcLanceRange = 4;
constexpr int kUrbanCell = 6;
constexpr int kUrbanFloor = 5;
constexpr double kDefaultZoom = 1.53;
constexpr double kActorScale = 1.0;

const wchar_t* kClassName = L"InfiniteIsoMiddleEarthWindow";
const std::vector<std::wstring> kRealms = {L"killer"};

struct Tile {
    std::wstring name = L"Grass";
};

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct Terrain {
    double elevation = 0.0;
    double moisture = 0.0;
    double forest = 0.0;
    double settlement = 0.0;
    double river = 1.0;
    double road = 1.0;
    double detail = 0.0;
    double scatter = 0.0;
};

struct RenderTile {
    int x = 0;
    int y = 0;
    int z = 0;
    Tile tile;
    std::wstring realm;
    bool anchoredToFeet = false;
    double scale = 1.0;
};

enum class OptionKind {
    Move,
    Tunnel,
    ArcLance,
    BreachCharge,
};

struct MoveOption {
    int x = 0;
    int y = 0;
    int z = 0;
    int dx = 0;
    int dy = 0;
    int dz = 0;
    OptionKind kind = OptionKind::Move;
    std::wstring facing = L"S";
};

struct Player {
    int x = 0;
    int y = 0;
    int z = 0;
    bool alive = true;
    std::wstring facing = L"S";
    std::wstring sprite = L"sphere-blue";
};

struct DroneController {
    int x = 0;
    int y = 0;
    int z = 0;
    bool alive = true;
};

struct AppState {
    HWND hwnd = nullptr;
    int width = 1280;
    int height = 720;
    double cameraX = 0.0;
    double cameraY = 0.0;
    double zoom = kDefaultZoom;
    double density = 0.52;
    std::wstring seedText = L"wanderer";
    uint32_t seedHash = 0;
    bool dragging = false;
    bool dragMoved = false;
    POINT dragStart = {};
    double dragCameraX = 0.0;
    double dragCameraY = 0.0;
    int turn = 0;
    int activePlayer = 0;
    int activeSphere = 0;
    int actionsThisPlayer = 0;
    Player players[kPlayers][kSpheresPerPlayer];
    DroneController controllers[kPlayers];
    int winner = -1;
    std::wstring winReason;
    int view[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    int viewTurns = 0;
    bool playerSelected = false;
    std::vector<MoveOption> moveOptions;
    std::unordered_map<std::wstring, std::unique_ptr<Bitmap>> images;
    std::unordered_set<std::wstring> removedBlocks;
    std::unordered_map<std::wstring, int> tunnelOwners;
};

AppState gApp;
ULONG_PTR gGdiToken = 0;

Player& ActivePlayer() {
    return gApp.players[gApp.activePlayer][gApp.activeSphere];
}

const Player& ActivePlayerConst() {
    return gApp.players[gApp.activePlayer][gApp.activeSphere];
}

int FirstLivingSphere(int playerIndex) {
    for (int i = 0; i < kSpheresPerPlayer; ++i) {
        if (gApp.players[playerIndex][i].alive) return i;
    }
    return 0;
}

int LivingSphereCount(int playerIndex) {
    int count = 0;
    for (int i = 0; i < kSpheresPerPlayer; ++i) {
        if (gApp.players[playerIndex][i].alive) ++count;
    }
    return count;
}

void SetActiveSphere(int sphereIndex) {
    if (sphereIndex < 0 || sphereIndex >= kSpheresPerPlayer) return;
    if (!gApp.players[gApp.activePlayer][sphereIndex].alive) return;
    gApp.activeSphere = sphereIndex;
}

void SetWinner(int playerIndex, const std::wstring& reason) {
    if (gApp.winner >= 0) return;
    gApp.winner = playerIndex;
    gApp.winReason = reason;
}

std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring full(path);
    const size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : full.substr(0, slash);
}

std::wstring ProjectDirectory() {
    std::wstring dir = ExeDirectory();
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos && dir.substr(slash + 1) == L"build") {
        return dir.substr(0, slash);
    }
    return dir;
}

uint32_t HashString(const std::wstring& value) {
    uint32_t hash = 2166136261u;
    for (wchar_t ch : value) {
        hash ^= static_cast<uint32_t>(ch);
        hash *= 16777619u;
    }
    return hash;
}

double Hash2(int x, int y, uint32_t salt) {
    uint32_t h = gApp.seedHash ^ static_cast<uint32_t>(x * 374761393) ^
                 static_cast<uint32_t>(y * 668265263) ^ (salt * 2246822519u);
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<double>(h) / static_cast<double>(UINT32_MAX);
}

double Smoothstep(double t) {
    return t * t * (3.0 - 2.0 * t);
}

double ValueNoise(double x, double y, double scale, uint32_t salt) {
    const double nx = x / scale;
    const double ny = y / scale;
    const int x0 = static_cast<int>(std::floor(nx));
    const int y0 = static_cast<int>(std::floor(ny));
    const double tx = Smoothstep(nx - x0);
    const double ty = Smoothstep(ny - y0);

    const double a = Hash2(x0, y0, salt);
    const double b = Hash2(x0 + 1, y0, salt);
    const double c = Hash2(x0, y0 + 1, salt);
    const double d = Hash2(x0 + 1, y0 + 1, salt);
    const double top = a + (b - a) * tx;
    const double bottom = c + (d - c) * tx;
    return top + (bottom - top) * ty;
}

double Fbm(double x, double y, uint32_t salt) {
    double amplitude = 0.56;
    double total = 0.0;
    double normalizer = 0.0;
    for (int octave = 0; octave < 4; ++octave) {
        total += ValueNoise(x, y, 7.0 * std::pow(2.0, octave), salt + octave * 17) * amplitude;
        normalizer += amplitude;
        amplitude *= 0.5;
    }
    return total / normalizer;
}

double Clamp(double value, double low, double high) {
    return std::max(low, std::min(high, value));
}

Tile PickWeighted(const std::vector<Tile>& tiles, double roll) {
    const size_t index = static_cast<size_t>(Clamp(roll, 0.0, 0.999999) * tiles.size());
    return tiles[index];
}

double IslandScore(int x, int y) {
    const double dx = static_cast<double>(x) / 15.0;
    const double dy = static_cast<double>(y) / 11.0;
    const double falloff = 1.0 - std::sqrt(dx * dx + dy * dy);
    const double lobeA = 0.42 - std::sqrt(std::pow((x + 9) / 9.0, 2.0) + std::pow((y - 5) / 7.0, 2.0));
    const double lobeB = 0.36 - std::sqrt(std::pow((x - 8) / 8.0, 2.0) + std::pow((y + 4) / 6.0, 2.0));
    const double rough = (Fbm(x * 0.55 + 30, y * 0.55 - 25, 700) - 0.5) * 0.24;
    return std::max(falloff, std::max(lobeA, lobeB)) + rough;
}

int PositiveMod(int value, int divisor) {
    const int result = value % divisor;
    return result < 0 ? result + divisor : result;
}

std::wstring BlockKey(int x, int y, int z) {
    wchar_t buffer[96] = {};
    swprintf_s(buffer, L"%d:%d:%d", x, y, z);
    return buffer;
}

bool ParseBlockKey(const std::wstring& key, int& x, int& y, int& z) {
    return swscanf_s(key.c_str(), L"%d:%d:%d", &x, &y, &z) == 3;
}

Terrain TerrainFor(int x, int y) {
    Terrain terrain;
    terrain.elevation = Fbm(x * 0.55 + 120, y * 0.55 - 90, 58);
    terrain.moisture = Fbm(x * 0.48 - 40, y * 0.48 + 75, 21);
    terrain.forest = Fbm(x * 0.62 + 260, y * 0.62 + 180, 36);
    terrain.settlement = Fbm(x * 0.36 - 310, y * 0.36 + 240, 74);
    terrain.detail = Hash2(x, y, 91);
    terrain.scatter = Hash2(x, y, 211);

    const double riverMeander = Fbm(x * 0.42 + 650, y * 0.42 - 420, 87);
    const double riverBend = std::sin((x * 0.036) + (y * 0.024) + riverMeander * 4.6);
    terrain.river = std::abs(riverBend);

    const double roadMeander = Fbm(x * 0.35 - 160, y * 0.35 - 250, 113);
    const double mainRoad = std::abs(std::sin((x - y) * 0.038 + roadMeander * 2.6));
    const double crossRoad = std::abs(std::sin((x + y) * 0.027 + roadMeander * 2.0));
    terrain.road = std::min(mainRoad, crossRoad + 0.11);
    return terrain;
}

double RawHeightFor(int x, int y) {
    const Terrain terrain = TerrainFor(x, y);
    const double score = IslandScore(x, y);
    if (score < 0.02) return 0.0;

    const double shore = Clamp(score / 0.48, 0.0, 1.0);
    const double massif = Fbm(x * 0.42 + 40, y * 0.42 - 80, 803);
    double height = shore * (1.0 + terrain.elevation * 2.8 + massif * 1.6);
    if (std::abs(x + y) < 3 && shore > 0.35) height -= 0.55;
    return Clamp(height, 0.0, 5.0);
}

int HeightFor(int x, int y) {
    if (x < 0 || x >= kWorldSize || y < 0 || y >= kWorldSize) return 0;
    const double ridge = Fbm(x * 0.7 + 240, y * 0.7 - 120, 1901);
    const double detail = Fbm(x * 1.4 - 90, y * 1.4 + 60, 1902);
    return 15 + static_cast<int>(std::floor((ridge * 0.72 + detail * 0.28) * 9.0));
}

bool InWorldCube(int x, int y, int z) {
    return x >= 0 && x < kWorldSize && y >= 0 && y < kWorldSize && z >= 0 && z < kWorldSize;
}

bool IsPlayableCell(int x, int y, int z) {
    return x >= -1 && x <= kWorldSize && y >= -1 && y <= kWorldSize && z >= -1 &&
           z <= kWorldSize;
}

int ReliefDepthForFace(int u, int v, uint32_t salt) {
    const double broad = Fbm(u * 0.52 + salt, v * 0.52 - salt * 0.43, salt + 3);
    const double detail = Fbm(u * 1.36 - salt * 0.17, v * 1.36 + salt * 0.29, salt + 29);
    const double ledge = Hash2(u / 2, v / 2, salt + 61);
    const double contrast = Smoothstep(Clamp(broad * 0.74 + detail * 0.26, 0.0, 1.0));
    const double shaped = contrast * 0.78 + detail * 0.12 + ledge * 0.10;
    return std::max(1, std::min(12, 1 + static_cast<int>(std::floor(shaped * 12.8))));
}

int TopSurfaceZ(int x, int y) {
    return kWorldSize - 1 - ReliefDepthForFace(x, y, 2100);
}

int BottomSurfaceZ(int x, int y) {
    return ReliefDepthForFace(x, y, 2200);
}

int WestSurfaceX(int y, int z) {
    return ReliefDepthForFace(y, z, 2300);
}

int EastSurfaceX(int y, int z) {
    return kWorldSize - 1 - ReliefDepthForFace(y, z, 2400);
}

int NorthSurfaceY(int x, int z) {
    return ReliefDepthForFace(x, z, 2500);
}

int SouthSurfaceY(int x, int z) {
    return kWorldSize - 1 - ReliefDepthForFace(x, z, 2600);
}

bool UrbanVoidAt(int x, int y, int z) {
    if (!InWorldCube(x, y, z)) return false;
    if (x < 3 || x > kWorldSize - 4 || y < 3 || y > kWorldSize - 4 || z < 3 ||
        z > kWorldSize - 5) {
        return false;
    }

    const int lx = PositiveMod(x, kUrbanCell);
    const int ly = PositiveMod(y, kUrbanCell);
    const int lz = PositiveMod(z, kUrbanFloor);
    const bool floorBand = lz == 1 || lz == 2;
    const bool upperBand = lz == 2 || lz == 3;

    const bool northSouthStreet = floorBand && (lx == 2 || lx == 3) && ly >= 1 && ly <= 4;
    const bool eastWestStreet = floorBand && (ly == 2 || ly == 3) && lx >= 1 && lx <= 4;
    const bool room = floorBand && lx >= 4 && lx <= 5 && ly >= 4 && ly <= 5 &&
                      Hash2(x / kUrbanCell, y / kUrbanCell + z / kUrbanFloor * 9, 3300) > 0.28;
    const bool marketPocket = upperBand && lx >= 1 && lx <= 2 && ly >= 4 && ly <= 5 &&
                              Hash2(x / kUrbanCell, y / kUrbanCell + z / kUrbanFloor * 11, 3400) > 0.68;
    const bool liftShaft = (lx == 1 && ly == 1) || (lx == 4 && ly == 1 && z > 6 && z < 18);
    const bool breach = (x == kWorldSize / 2 || x == kWorldSize / 2 - 1) &&
                        (y == kWorldSize / 2 || y == kWorldSize / 2 - 1) && z >= 7 && z <= 17;

    return northSouthStreet || eastWestStreet || room || marketPocket || liftShaft || breach;
}

bool UrbanSolidWallAt(int x, int y, int z) {
    if (!InWorldCube(x, y, z) || UrbanVoidAt(x, y, z)) return false;
    const int lx = PositiveMod(x, kUrbanCell);
    const int ly = PositiveMod(y, kUrbanCell);
    const int lz = PositiveMod(z, kUrbanFloor);
    return lx == 0 || ly == 0 || lz == 0 || (lx == 5 && ly <= 3) || (ly == 5 && lx <= 3);
}

bool NaturalSolidBlockAt(int x, int y, int z) {
    if (!InWorldCube(x, y, z)) return false;
    if (z > TopSurfaceZ(x, y)) return false;
    if (z < BottomSurfaceZ(x, y)) return false;
    if (x < WestSurfaceX(y, z)) return false;
    if (x > EastSurfaceX(y, z)) return false;
    if (y < NorthSurfaceY(x, z)) return false;
    if (y > SouthSurfaceY(x, z)) return false;
    if (UrbanVoidAt(x, y, z)) return false;
    return true;
}

bool SolidBlockAt(int x, int y, int z) {
    if (!NaturalSolidBlockAt(x, y, z)) return false;
    return gApp.removedBlocks.find(BlockKey(x, y, z)) == gApp.removedBlocks.end();
}

bool DugCellAt(int x, int y, int z) {
    return InWorldCube(x, y, z) &&
           gApp.removedBlocks.find(BlockKey(x, y, z)) != gApp.removedBlocks.end();
}

int TunnelOwnerAt(int x, int y, int z) {
    const auto it = gApp.tunnelOwners.find(BlockKey(x, y, z));
    return it == gApp.tunnelOwners.end() ? -1 : it->second;
}

bool ControllerAt(int playerIndex, int x, int y, int z) {
    if (playerIndex < 0 || playerIndex >= kPlayers) return false;
    const DroneController& controller = gApp.controllers[playerIndex];
    return controller.alive && controller.x == x && controller.y == y && controller.z == z;
}

bool AnyControllerAt(int x, int y, int z, int* owner = nullptr) {
    for (int playerIndex = 0; playerIndex < kPlayers; ++playerIndex) {
        if (ControllerAt(playerIndex, x, y, z)) {
            if (owner) *owner = playerIndex;
            return true;
        }
    }
    return false;
}

bool OccupyingSphereAt(int x, int y, int z, int* owner = nullptr, int* sphere = nullptr) {
    for (int playerIndex = 0; playerIndex < kPlayers; ++playerIndex) {
        for (int sphereIndex = 0; sphereIndex < kSpheresPerPlayer; ++sphereIndex) {
            const Player& player = gApp.players[playerIndex][sphereIndex];
            if (!player.alive) continue;
            if (player.x == x && player.y == y && player.z == z) {
                if (owner) *owner = playerIndex;
                if (sphere) *sphere = sphereIndex;
                return true;
            }
        }
    }
    return false;
}

bool ControllerRevealed(int playerIndex) {
    if (playerIndex < 0 || playerIndex >= kPlayers || !gApp.controllers[playerIndex].alive) return false;
    const DroneController& controller = gApp.controllers[playerIndex];
    if (!SolidBlockAt(controller.x, controller.y, controller.z)) return true;

    const struct Direction {
        int dx;
        int dy;
        int dz;
    } directions[] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
    };
    for (const Direction& direction : directions) {
        if (InWorldCube(controller.x + direction.dx, controller.y + direction.dy,
                        controller.z + direction.dz) &&
            !SolidBlockAt(controller.x + direction.dx, controller.y + direction.dy,
                          controller.z + direction.dz)) {
            return true;
        }
    }
    return false;
}

bool ExposedBlockAt(int x, int y, int z) {
    if (!SolidBlockAt(x, y, z)) return false;

    const struct Direction {
        int dx;
        int dy;
        int dz;
    } directions[] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
    };

    for (const Direction& direction : directions) {
        if (!SolidBlockAt(x + direction.dx, y + direction.dy, z + direction.dz)) {
            return true;
        }
    }
    return false;
}

bool HasAnyBlockInColumn(int x, int y) {
    const int height = HeightFor(x, y);
    for (int z = 0; z < height; ++z) {
        if (SolidBlockAt(x, y, z)) return true;
    }
    return false;
}

void RemoveBlockAt(int x, int y, int z) {
    if (InWorldCube(x, y, z)) {
        const std::wstring key = BlockKey(x, y, z);
        gApp.removedBlocks.insert(key);
        gApp.tunnelOwners[key] = gApp.activePlayer;
    }
}

Tile SideTileFor(int x, int y, int z, int height) {
    const Terrain terrain = TerrainFor(x, y);
    const double roll = Hash2(x + z * 13, y - z * 17, 820);
    const double score = IslandScore(x, y);
    const bool ruin = terrain.settlement > 0.66 && score > 0.57 && height >= 3;

    if (ruin && z >= height - 2) {
        if (terrain.moisture > 0.60 && roll > 0.45) return {L"Cutsone_Moss"};
        return roll > 0.82 ? Tile{L"Cutstone_Broken"} : Tile{L"Cutstone"};
    }

    if (z < height - 2) {
        if (terrain.elevation > 0.70 && terrain.moisture > 0.50) return {L"SnowCM_Stone"};
        return roll > 0.18 ? Tile{L"Stone"} : Tile{L"Stone_Dirt"};
    }

    if (terrain.moisture < 0.30 && score < 0.60) return {L"Dirt"};
    if (terrain.elevation > 0.64) return roll > 0.45 ? Tile{L"Stone_Grass"} : Tile{L"Stone"};
    return roll > 0.40 ? Tile{L"Dirt_Grass"} : Tile{L"Dirt"};
}

Tile TopTileFor(int x, int y, int height) {
    const Terrain terrain = TerrainFor(x, y);
    const double densityGate = 1.0 - gApp.density * 0.42;
    const double score = IslandScore(x, y);
    const bool edge = HeightFor(x - 1, y) == 0 || HeightFor(x + 1, y) == 0 ||
                      HeightFor(x, y - 1) == 0 || HeightFor(x, y + 1) == 0;

    (void)height;
    const bool cold = terrain.elevation > 0.78 && terrain.moisture > 0.46;
    const bool dry = terrain.moisture < 0.30;
    const bool rocky = terrain.elevation > 0.66;

    if (cold) {
        if (rocky && terrain.detail > 0.55) return {L"SnowCM_Stone"};
        if (terrain.moisture > 0.58 && terrain.detail > 0.45) return {L"Snow_Grass"};
        if (terrain.detail > 0.84) return {L"Snow_Packed"};
        return {L"Snow"};
    }

    if (rocky) {
        if (terrain.moisture > 0.48 && terrain.detail > 0.28) return {L"Stone_Grass"};
        if (terrain.detail > 0.68) return {L"Stone_Dirt"};
        return {L"Stone"};
    }

    if (dry || (edge && terrain.moisture < 0.42)) {
        if (terrain.detail > 0.83) return {L"Dirt_Smooth"};
        return {L"Sand"};
    }

    if (terrain.road < 0.045 && score > 0.57) {
        return terrain.detail > 0.50 ? Tile{L"Wood"} : Tile{L"Dirt_Smooth"};
    }

    if (terrain.moisture > 0.63 && terrain.detail > 0.50) {
        return {L"Grass_Full"};
    }

    if (terrain.forest > densityGate && terrain.detail > 0.70 && !edge) {
        return terrain.scatter > 0.50 ? Tile{L"Bush_Twigs"} : Tile{L"Bush"};
    }

    if (edge && terrain.moisture > 0.45) return {L"Grass_Dirt"};
    if (terrain.detail > 0.76) return {L"Grass_Dirt"};
    return terrain.detail > 0.30 ? Tile{L"Grass"} : Tile{L"Dirt_Grass"};
}

Tile LandscapeTileForFace(int u, int v, int altitude, bool vertical, uint32_t salt) {
    const double elevation = Fbm(u * 0.58 + salt, v * 0.58 - salt * 0.7, salt + 11);
    const double moisture = Fbm(u * 0.46 - salt * 0.3, v * 0.46 + salt, salt + 37);
    const double detail = Hash2(u + static_cast<int>(salt), v - static_cast<int>(salt), salt + 71);
    const double vein = Fbm(u * 0.9 + altitude * 1.7, v * 0.9 - altitude * 1.3, salt + 131);
    const double high = static_cast<double>(altitude) / static_cast<double>(kWorldSize - 1);

    if (vertical) {
        if (vein > 0.78) return moisture > 0.54 ? Tile{L"Cutsone_Moss"} : Tile{L"Cutstone"};
        if (high > 0.76 && moisture > 0.58) return detail > 0.45 ? Tile{L"Grass_Dirt"} : Tile{L"Dirt_Grass"};
        if (high > 0.64 && moisture > 0.50 && detail > 0.38) return Tile{L"Stone_Grass"};
        if (elevation > 0.68) return detail > 0.55 ? Tile{L"Stone_Dirt"} : Tile{L"Stone"};
        if (moisture < 0.28) return Tile{L"Dirt"};
        return detail > 0.50 ? Tile{L"Dirt_Grass"} : Tile{L"Stone_Dirt"};
    }

    if (high < 0.08) return detail > 0.35 ? Tile{L"Stone"} : Tile{L"Stone_Dirt"};
    if (elevation > 0.78 && moisture > 0.46) return detail > 0.58 ? Tile{L"SnowCM_Stone"} : Tile{L"Snow"};
    if (moisture < 0.26) return detail > 0.72 ? Tile{L"Dirt_Smooth"} : Tile{L"Sand"};
    if (elevation > 0.66) return moisture > 0.48 ? Tile{L"Stone_Grass"} : Tile{L"Stone"};
    if (moisture > 0.64 && detail > 0.54) return Tile{L"Grass_Full"};
    if (detail > 0.80) return Tile{L"Grass_Dirt"};
    return detail > 0.34 ? Tile{L"Grass"} : Tile{L"Dirt_Grass"};
}

Tile SurfaceTileFor(int x, int y, int z) {
    if (UrbanSolidWallAt(x, y, z)) {
        const double roll = Hash2(x + z * 5, y - z * 7, 3700);
        const int lz = PositiveMod(z, kUrbanFloor);
        if (lz == 0 && roll > 0.58) return {L"Wood"};
        if (roll > 0.76) return {L"Cutstone_Broken"};
        if (roll > 0.46) return {L"Cutsone_Moss"};
        return {L"Cutstone"};
    }
    if (!SolidBlockAt(x, y, z + 1)) return LandscapeTileForFace(x, y, z, false, 900);
    if (!SolidBlockAt(x - 1, y, z)) return LandscapeTileForFace(y, z, z, true, 1020);
    if (!SolidBlockAt(x + 1, y, z)) return LandscapeTileForFace(y, z, z, true, 1140);
    if (!SolidBlockAt(x, y - 1, z)) return LandscapeTileForFace(x, z, z, true, 1260);
    if (!SolidBlockAt(x, y + 1, z)) return LandscapeTileForFace(x, z, z, true, 1380);
    if (!SolidBlockAt(x, y, z - 1)) return LandscapeTileForFace(x, y, z, false, 1500);
    return LandscapeTileForFace(x, y, z, false, 1620);
}

std::wstring RealmFor(int x, int y) {
    (void)x;
    (void)y;
    return L"killer";
}

std::wstring TilePath(const std::wstring& realm, Tile tile) {
    return realm + L":" + tile.name;
}

std::wstring TileKey(const std::wstring& realm, Tile tile) {
    return TilePath(realm, tile);
}

Bitmap* ImageFor(const std::wstring& realm, Tile tile) {
    const std::wstring primary = TilePath(realm, tile);
    auto it = gApp.images.find(primary);
    if (it != gApp.images.end()) return it->second.get();

    const std::wstring fallback = TilePath(realm, {L"Grass"});
    it = gApp.images.find(fallback);
    return it == gApp.images.end() ? nullptr : it->second.get();
}

std::unique_ptr<Bitmap> CropToAlpha(Bitmap* source) {
    if (!source) return nullptr;

    int minX = static_cast<int>(source->GetWidth());
    int minY = static_cast<int>(source->GetHeight());
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < static_cast<int>(source->GetHeight()); ++y) {
        for (int x = 0; x < static_cast<int>(source->GetWidth()); ++x) {
            Color pixel;
            if (source->GetPixel(x, y, &pixel) == Status::Ok && pixel.GetAlpha() > 8) {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
    }

    if (maxX < minX || maxY < minY) return nullptr;
    constexpr int pad = 2;
    minX = std::max(0, minX - pad);
    minY = std::max(0, minY - pad);
    maxX = std::min(static_cast<int>(source->GetWidth()) - 1, maxX + pad);
    maxY = std::min(static_cast<int>(source->GetHeight()) - 1, maxY + pad);

    Rect sourceRect(minX, minY, maxX - minX + 1, maxY - minY + 1);
    return std::unique_ptr<Bitmap>(source->Clone(sourceRect, PixelFormat32bppPARGB));
}

std::unique_ptr<Bitmap> CreateSphereSprite(BYTE baseR, BYTE baseG, BYTE baseB) {
    constexpr int size = 18;
    auto sprite = std::make_unique<Bitmap>(size, size, PixelFormat32bppPARGB);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double nx = (x + 0.5 - size * 0.5) / (size * 0.44);
            const double ny = (y + 0.5 - size * 0.5) / (size * 0.44);
            const double distance = std::sqrt(nx * nx + ny * ny);
            if (distance > 1.0) {
                sprite->SetPixel(x, y, Color(0, 0, 0, 0));
                continue;
            }

            const double highlight = Clamp(1.0 - std::sqrt(std::pow(nx + 0.38, 2.0) +
                                                           std::pow(ny + 0.48, 2.0)),
                                           0.0, 1.0);
            const double shade = Clamp(1.0 - distance * 0.72 + highlight * 0.38, 0.0, 1.0);
            const BYTE r = static_cast<BYTE>(Clamp(baseR * 0.45 + shade * baseR * 0.80, 0.0, 255.0));
            const BYTE g = static_cast<BYTE>(Clamp(baseG * 0.45 + shade * baseG * 0.80, 0.0, 255.0));
            const BYTE b = static_cast<BYTE>(Clamp(baseB * 0.45 + shade * baseB * 0.80, 0.0, 255.0));
            sprite->SetPixel(x, y, Color(255, r, g, b));
        }
    }
    return sprite;
}

void LoadImages() {
    gApp.images.clear();
    const std::vector<std::wstring> names = {
        L"Bush",          L"Bush_Twigs",       L"Cutsone_Moss",       L"Cutstone",
        L"Cutstone_Broken", L"Dirt",           L"Dirt_Grass",         L"Dirt_Smooth",
        L"Grass",        L"Grass_Dirt",       L"Grass_Full",         L"Sand",
        L"Snow",         L"Snow_Grass",       L"Snow_Packed",        L"SnowC_Bush",
        L"SnowC_Cutstone", L"SnowC_Dirt",     L"SnowC_Grass_Full",   L"SnowC_Stone",
        L"SnowC_Wood",   L"SnowCM_Bush",      L"SnowCM_Cutstone",    L"SnowCM_Dirt",
        L"SnowCM_Grass_Full", L"SnowCM_Stone", L"SnowCM_Wood",       L"Stone",
        L"Stone_Dirt",   L"Stone_Grass",      L"Wood"};

    const std::wstring tileDir = ProjectDirectory() + L"\\KillerTiles\\";
    for (const std::wstring& name : names) {
        const std::wstring path = tileDir + name + L".png";
        std::unique_ptr<Bitmap> image(Bitmap::FromFile(path.c_str(), FALSE));
        if (image && image->GetLastStatus() == Status::Ok) {
            gApp.images.emplace(TilePath(L"killer", {name}), std::move(image));
        }
    }

    gApp.images.emplace(TilePath(L"actor", {L"sphere-blue"}), CreateSphereSprite(80, 150, 235));
    gApp.images.emplace(TilePath(L"actor", {L"sphere-red"}), CreateSphereSprite(235, 70, 70));

    if (gApp.images.empty()) {
        MessageBoxW(gApp.hwnd, tileDir.c_str(), L"Could not load Killer isometric tiles", MB_ICONERROR);
    }
}

Vec3 ViewTransform(double x, double y, double z) {
    return {
        gApp.view[0] * x + gApp.view[1] * y + gApp.view[2] * z,
        gApp.view[3] * x + gApp.view[4] * y + gApp.view[5] * z,
        gApp.view[6] * x + gApp.view[7] * y + gApp.view[8] * z,
    };
}

double ViewHalfH() {
    return 8.0;
}

double ViewLayerH() {
    return 16.0;
}

double ViewDepth(int x, int y, int z) {
    const Vec3 view = ViewTransform(x, y, z);
    return (view.x + view.y) * 32.0 + view.z * 36.0;
}

PointF WorldToScreen3(int x, int y, int z) {
    const Vec3 view = ViewTransform(x, y, z);
    return PointF(
        static_cast<Gdiplus::REAL>(gApp.width / 2.0 + (view.x - view.y) * kHalfW * gApp.zoom -
                                   gApp.cameraX),
        static_cast<Gdiplus::REAL>(gApp.height / 2.0 + ((view.x + view.y) * ViewHalfH() -
                                                        view.z * ViewLayerH()) *
                                                           gApp.zoom -
                                   gApp.cameraY));
}

PointF WorldToScreen(int x, int y) {
    return WorldToScreen3(x, y, 0);
}

Vec2 ScreenToWorld(double x, double y) {
    const double relX = (x - gApp.width / 2.0 + gApp.cameraX) / gApp.zoom;
    const double relY = (y - gApp.height / 2.0 + gApp.cameraY) / gApp.zoom;
    const double isoX = (relX / kHalfW + relY / ViewHalfH()) * 0.5;
    const double isoY = (relY / ViewHalfH() - relX / kHalfW) * 0.5;
    const double wx = gApp.view[0] * isoX + gApp.view[3] * isoY;
    const double wy = gApp.view[1] * isoX + gApp.view[4] * isoY;
    return {std::floor(wx), std::floor(wy)};
}

void OffsetPointY(PointF& point, float amount) {
    point.Y += amount;
}

void CellDiamondPoints(int x, int y, int z, PointF (&points)[4]) {
    const PointF center = WorldToScreen3(x, y, z);
    const float halfW = static_cast<float>(kHalfW * gApp.zoom);
    const float halfH = static_cast<float>(ViewHalfH() * gApp.zoom);
    points[0] = PointF(center.X, center.Y - halfH);
    points[1] = PointF(center.X + halfW, center.Y);
    points[2] = PointF(center.X, center.Y + halfH);
    points[3] = PointF(center.X - halfW, center.Y);
}

bool PointInCellDiamond(int screenX, int screenY, int cellX, int cellY, int cellZ) {
    const PointF center = WorldToScreen3(cellX, cellY, cellZ);
    const double halfW = kHalfW * gApp.zoom;
    const double halfH = ViewHalfH() * gApp.zoom;
    const double dx = std::abs((screenX - center.X) / halfW);
    const double dy = std::abs((screenY - center.Y) / halfH);
    return dx + dy <= 1.08;
}

bool IsForceOption(OptionKind kind) {
    return kind == OptionKind::Tunnel || kind == OptionKind::ArcLance || kind == OptionKind::BreachCharge;
}

void DrawOptionHighlight(Graphics& graphics, const MoveOption& option) {
    PointF points[4] = {
        PointF(), PointF(), PointF(), PointF(),
    };
    CellDiamondPoints(option.x, option.y, option.z, points);

    Color fill = Color(125, 80, 220, 120);
    Color line = Color(225, 120, 255, 180);
    float width = 2.0f;
    if (option.kind == OptionKind::Tunnel) {
        fill = Color(145, 255, 135, 42);
        line = Color(235, 255, 175, 70);
    } else if (option.kind == OptionKind::ArcLance) {
        fill = Color(170, 255, 42, 64);
        line = Color(250, 255, 110, 130);
        width = 3.0f;
    } else if (option.kind == OptionKind::BreachCharge) {
        fill = Color(150, 255, 180, 54);
        line = Color(240, 255, 220, 100);
        width = 2.6f;
    }
    SolidBrush brush(fill);
    Pen pen(line, width);
    if (option.kind == OptionKind::ArcLance) {
        pen.SetDashStyle(Gdiplus::DashStyleDash);
    }
    if (option.kind == OptionKind::ArcLance || option.kind == OptionKind::BreachCharge) {
        const Player& player = ActivePlayerConst();
        const PointF start = WorldToScreen3(player.x, player.y, player.z);
        const PointF end = WorldToScreen3(option.x, option.y, option.z);
        Pen beam(option.kind == OptionKind::ArcLance ? Color(180, 255, 74, 95)
                                                     : Color(135, 255, 180, 60),
                 option.kind == OptionKind::ArcLance ? 2.5f : 2.0f);
        if (option.kind == OptionKind::BreachCharge) {
            beam.SetDashStyle(Gdiplus::DashStyleDot);
        }
        graphics.DrawLine(&beam, start, end);
    }
    graphics.FillPolygon(&brush, points, 4);
    graphics.DrawPolygon(&pen, points, 4);

    if (IsForceOption(option.kind)) {
        const PointF center = WorldToScreen3(option.x, option.y, option.z);
        const float r = static_cast<float>((option.kind == OptionKind::ArcLance ? 4.5 : 3.5) * gApp.zoom);
        SolidBrush core(option.kind == OptionKind::ArcLance ? Color(235, 255, 220, 180)
                                                            : Color(210, 255, 198, 95));
        graphics.FillEllipse(&core, RectF(center.X - r, center.Y - r, r * 2.0f, r * 2.0f));
    }
}

Color TunnelFillColor(int owner, bool occupied) {
    if (owner == 0) return occupied ? Color(235, 18, 44, 94) : Color(205, 8, 22, 72);
    if (owner == 1) return occupied ? Color(235, 82, 20, 28) : Color(205, 62, 9, 18);
    return occupied ? Color(230, 24, 42, 72) : Color(205, 8, 18, 34);
}

Color TunnelEdgeColor(int owner, bool occupied) {
    if (owner == 0) return occupied ? Color(255, 170, 230, 255) : Color(245, 80, 165, 255);
    if (owner == 1) return occupied ? Color(255, 255, 180, 180) : Color(245, 255, 105, 105);
    return occupied ? Color(255, 240, 210, 90) : Color(245, 115, 210, 255);
}

Color TunnelInnerColor(int owner, bool occupied) {
    if (owner == 0) return occupied ? Color(255, 220, 245, 255) : Color(230, 135, 210, 255);
    if (owner == 1) return occupied ? Color(255, 255, 220, 220) : Color(230, 255, 150, 150);
    return occupied ? Color(255, 255, 245, 150) : Color(230, 150, 235, 255);
}

Color TunnelGlowColor(int owner) {
    if (owner == 0) return Color(95, 20, 70, 140);
    if (owner == 1) return Color(95, 90, 18, 26);
    return Color(95, 25, 70, 130);
}

void DrawTunnelCell(Graphics& graphics, int x, int y, int z, bool occupied) {
    PointF points[4] = {PointF(), PointF(), PointF(), PointF()};
    CellDiamondPoints(x, y, z, points);

    const int owner = TunnelOwnerAt(x, y, z);
    const Color fill = TunnelFillColor(owner, occupied);
    const Color edge = TunnelEdgeColor(owner, occupied);
    const Color inner = TunnelInnerColor(owner, occupied);
    SolidBrush brush(fill);
    Pen pen(edge, occupied ? 3.6f : 2.4f);
    Pen innerPen(inner, occupied ? 2.0f : 1.2f);
    graphics.FillPolygon(&brush, points, 4);
    graphics.DrawPolygon(&pen, points, 4);

    const PointF center = WorldToScreen3(x, y, z);
    const float insetW = static_cast<float>(kHalfW * 0.42 * gApp.zoom);
    const float insetH = static_cast<float>(ViewHalfH() * 0.42 * gApp.zoom);
    PointF innerDiamond[4] = {
        PointF(center.X, center.Y - insetH),
        PointF(center.X + insetW, center.Y),
        PointF(center.X, center.Y + insetH),
        PointF(center.X - insetW, center.Y),
    };
    SolidBrush mouthBrush(occupied ? Color(245, 8, 12, 22) : Color(225, 3, 7, 13));
    graphics.FillPolygon(&mouthBrush, innerDiamond, 4);
    graphics.DrawPolygon(&innerPen, innerDiamond, 4);

    const float drop = static_cast<float>(ViewLayerH() * 0.72 * gApp.zoom);
    PointF lower[4] = {
        PointF(innerDiamond[0].X, innerDiamond[0].Y + drop),
        PointF(innerDiamond[1].X, innerDiamond[1].Y + drop),
        PointF(innerDiamond[2].X, innerDiamond[2].Y + drop),
        PointF(innerDiamond[3].X, innerDiamond[3].Y + drop),
    };
    Pen shaftPen(occupied ? TunnelInnerColor(owner, true) : TunnelEdgeColor(owner, false), 1.5f);
    graphics.DrawLine(&shaftPen, innerDiamond[1], lower[1]);
    graphics.DrawLine(&shaftPen, innerDiamond[2], lower[2]);
    graphics.DrawLine(&shaftPen, innerDiamond[3], lower[3]);
    graphics.DrawPolygon(&shaftPen, lower, 4);

    const struct Direction {
        int dx;
        int dy;
        int dz;
    } directions[] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
    };
    Pen glowPen(TunnelGlowColor(owner), occupied ? 6.0f : 4.0f);
    for (const Direction& direction : directions) {
        const int neighborOwner = TunnelOwnerAt(x + direction.dx, y + direction.dy, z + direction.dz);
        const bool playerInNeighbor =
            InWorldCube(ActivePlayerConst().x, ActivePlayerConst().y, ActivePlayerConst().z) &&
            !SolidBlockAt(ActivePlayerConst().x, ActivePlayerConst().y, ActivePlayerConst().z) &&
            x + direction.dx == ActivePlayerConst().x && y + direction.dy == ActivePlayerConst().y &&
            z + direction.dz == ActivePlayerConst().z;
        if (!DugCellAt(x + direction.dx, y + direction.dy, z + direction.dz) &&
            !playerInNeighbor) {
            continue;
        }
        const PointF neighbor = WorldToScreen3(x + direction.dx, y + direction.dy, z + direction.dz);
        PointF start(center.X + (neighbor.X - center.X) * 0.22f,
                     center.Y + (neighbor.Y - center.Y) * 0.22f);
        PointF end(center.X + (neighbor.X - center.X) * 0.50f,
                   center.Y + (neighbor.Y - center.Y) * 0.50f);
        const bool mixedOwners = neighborOwner >= 0 && owner >= 0 && neighborOwner != owner;
        Pen linkPen(mixedOwners ? Color(245, 245, 225, 90) : TunnelEdgeColor(owner, occupied),
                    occupied ? 3.0f : 2.0f);
        if (mixedOwners) {
            linkPen.SetDashStyle(Gdiplus::DashStyleDash);
        }
        graphics.DrawLine(&glowPen, start, end);
        graphics.DrawLine(&linkPen, start, end);
    }
}

void DrawTerrainImage(Graphics& graphics, Bitmap* image, const RectF& dest) {
    if (!image) return;

    const ColorMatrix matrix = {{
        {0.78f, 0.06f, 0.05f, 0.0f, 0.0f},
        {0.08f, 0.48f, 0.03f, 0.0f, 0.0f},
        {0.15f, 0.06f, 0.34f, 0.0f, 0.0f},
        {0.0f,  0.0f,  0.0f,  1.0f, 0.0f},
        {0.20f, 0.04f, 0.01f, 0.0f, 1.0f},
    }};

    ImageAttributes attrs;
    attrs.SetColorMatrix(&matrix);
    graphics.DrawImage(image, dest,
                       0.0f, 0.0f,
                       static_cast<Gdiplus::REAL>(image->GetWidth()),
                       static_cast<Gdiplus::REAL>(image->GetHeight()),
                       Gdiplus::UnitPixel,
                       &attrs);
}

void DrawPlayerSphere(Graphics& graphics, int playerIndex, int sphereIndex) {
    const Player& player = gApp.players[playerIndex][sphereIndex];
    if (!player.alive) return;
    Bitmap* image = ImageFor(L"actor", {player.sprite});
    const float drawW = static_cast<float>((image ? image->GetWidth() : 18) * kActorScale * gApp.zoom);
    const float drawH = static_cast<float>((image ? image->GetHeight() : 18) * kActorScale * gApp.zoom);
    const PointF pos = WorldToScreen3(player.x, player.y, player.z);
    const RectF bounds(pos.X - drawW * 0.5f,
                       pos.Y + static_cast<float>(ViewHalfH() * gApp.zoom) - drawH, drawW, drawH);
    const bool underground = InWorldCube(player.x, player.y, player.z) &&
                             NaturalSolidBlockAt(player.x, player.y, player.z);

    const bool activeSphere = playerIndex == gApp.activePlayer && sphereIndex == gApp.activeSphere;
    if (underground || activeSphere) {
        RectF halo(bounds.X - 4.0f * static_cast<float>(gApp.zoom),
                   bounds.Y - 4.0f * static_cast<float>(gApp.zoom),
                   bounds.Width + 8.0f * static_cast<float>(gApp.zoom),
                   bounds.Height + 8.0f * static_cast<float>(gApp.zoom));
        SolidBrush glow(activeSphere ? Color(120, 250, 230, 120) : Color(90, 85, 180, 255));
        Pen ring(playerIndex == 0 ? Color(235, 170, 220, 255) : Color(235, 255, 150, 150), 2.0f);
        graphics.FillEllipse(&glow, halo);
        graphics.DrawEllipse(&ring, halo);
    }

    if (image) {
        graphics.DrawImage(image, bounds);
    }
}

void DrawControllerCore(Graphics& graphics, int playerIndex) {
    if (!ControllerRevealed(playerIndex)) return;

    const DroneController& controller = gApp.controllers[playerIndex];
    PointF points[4] = {PointF(), PointF(), PointF(), PointF()};
    CellDiamondPoints(controller.x, controller.y, controller.z, points);

    const bool blue = playerIndex == 0;
    SolidBrush footprint(blue ? Color(160, 20, 70, 120) : Color(160, 120, 18, 28));
    Pen outer(blue ? Color(245, 105, 205, 255) : Color(245, 255, 95, 95), 2.8f);
    Pen inner(Color(235, 255, 222, 130), 1.4f);
    graphics.FillPolygon(&footprint, points, 4);
    graphics.DrawPolygon(&outer, points, 4);

    const PointF center = WorldToScreen3(controller.x, controller.y, controller.z);
    const float r = static_cast<float>(7.0 * gApp.zoom);
    SolidBrush glow(blue ? Color(190, 90, 190, 255) : Color(190, 255, 80, 80));
    SolidBrush hot(Color(235, 255, 220, 125));
    graphics.FillEllipse(&glow, RectF(center.X - r, center.Y - r * 1.15f, r * 2.0f, r * 2.0f));
    graphics.FillEllipse(&hot, RectF(center.X - r * 0.42f, center.Y - r * 0.56f,
                                     r * 0.84f, r * 0.84f));
    graphics.DrawEllipse(&inner, RectF(center.X - r, center.Y - r * 1.15f, r * 2.0f, r * 2.0f));
}

void DrawHud(Graphics& graphics, int visibleCount) {
    SolidBrush panel(Color(210, 12, 18, 16));
    SolidBrush muted(Color(185, 194, 171));
    SolidBrush hot(Color(230, 255, 190, 110));
    FontFamily sans(L"Segoe UI");
    Font hudFont(&sans, 13.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

    graphics.FillRectangle(&panel, RectF(18, 18, static_cast<float>(gApp.width - 36), 44));

    wchar_t stats[256] = {};
    const Player& player = ActivePlayerConst();
    swprintf_s(stats, L"cube: %dx%dx%d    turn: %d    active: P%d.%d action:%d/2    drones: %d-%d    ctrl: %c/%c    sphere: %d,%d,%d    visible: %d    zoom: %.2fx",
               kWorldSize, kWorldSize, kWorldSize,
               gApp.turn, gApp.activePlayer + 1, gApp.activeSphere + 1, gApp.actionsThisPlayer + 1,
               LivingSphereCount(0), LivingSphereCount(1),
               gApp.controllers[0].alive ? L'A' : L'X',
               gApp.controllers[1].alive ? L'A' : L'X',
               player.x, player.y, player.z,
               visibleCount, gApp.zoom);
    graphics.DrawString(stats, -1, &hudFont, PointF(34, 32), &muted);
    if (gApp.winner >= 0) {
        wchar_t winText[192] = {};
        swprintf_s(winText, L"P%d wins: %s", gApp.winner + 1, gApp.winReason.c_str());
        graphics.DrawString(winText, -1, &hudFont, PointF(static_cast<float>(gApp.width) * 0.50f, 32),
                            &hot);
    }

    graphics.FillRectangle(&panel, RectF(18, static_cast<float>(gApp.height - 64), 520, 46));
    graphics.DrawString(L"Green move | Orange tunnel | Red arc-lance | Gold breach charge | Kill drones or controller",
                        -1, &hudFont, PointF(34, static_cast<float>(gApp.height - 50)), &muted);
}

void DrawHellscapeBackgroundLayer(Graphics& graphics, float width, float height) {
    const RectF skyRect(0, 0, width, height);
    LinearGradientBrush sky(skyRect, Color(255, 0, 2, 9), Color(255, 34, 7, 5),
                            Gdiplus::LinearGradientModeVertical);
    graphics.FillRectangle(&sky, skyRect);

    LinearGradientBrush horizon(skyRect, Color(0, 0, 0, 0), Color(90, 105, 18, 6),
                             Gdiplus::LinearGradientModeVertical);
    graphics.FillRectangle(&horizon, skyRect);

    const float planetX = width * 0.70f;
    const float planetY = height * 0.42f;
    const float planetR = std::min(width, height) * 0.46f;
    SolidBrush planetOuterGlow(Color(34, 150, 42, 10));
    SolidBrush planetGlow(Color(58, 135, 28, 8));
    SolidBrush planetBody(Color(92, 70, 14, 7));
    SolidBrush planetHotCore(Color(70, 120, 28, 8));
    graphics.FillEllipse(&planetOuterGlow, RectF(planetX - planetR * 1.82f, planetY - planetR * 1.82f,
                                                planetR * 3.64f, planetR * 3.64f));
    graphics.FillEllipse(&planetGlow, RectF(planetX - planetR * 1.18f, planetY - planetR * 1.18f,
                                           planetR * 2.36f, planetR * 2.36f));
    graphics.FillEllipse(&planetBody, RectF(planetX - planetR, planetY - planetR,
                                           planetR * 2.0f, planetR * 2.0f));
    graphics.FillEllipse(&planetHotCore, RectF(planetX - planetR * 0.86f, planetY - planetR * 0.83f,
                                              planetR * 1.72f, planetR * 1.54f));

    SolidBrush eclipse(Color(238, 0, 1, 5));
    graphics.FillEllipse(&eclipse, RectF(planetX - planetR * 0.35f, planetY - planetR * 1.06f,
                                        planetR * 0.94f, planetR * 0.94f));

    for (int i = 0; i < 22; ++i) {
        const float y = static_cast<float>(height * (0.18 + Hash2(i, 1, 4100) * 0.58));
        const float x = static_cast<float>(Hash2(i, 2, 4101) * width);
        const float w = static_cast<float>(width * (0.58 + Hash2(i, 3, 4102) * 1.05));
        const float h = static_cast<float>(height * (0.025 + Hash2(i, 4, 4103) * 0.105));
        SolidBrush band(Hash2(i, 5, 4104) > 0.40 ? Color(42, 140, 32, 10)
                                                 : Color(48, 18, 5, 10));
        graphics.FillEllipse(&band, RectF(x - w * 0.5f, y, w, h));
    }

    for (int i = 0; i < 20; ++i) {
        const float y = static_cast<float>(height * (0.25 + i * 0.026));
        Pen heatBand(Color(38, 120, 26, 8), static_cast<float>(4.0 + Hash2(i, 7, 4200) * 14.0));
        graphics.DrawLine(&heatBand, PointF(-width * 0.12f, y),
                          PointF(width * 1.12f, y + static_cast<float>(Hash2(i, 9, 4202) * 22.0 - 11.0)));
    }

    SolidBrush ridgeBack(Color(120, 2, 3, 5));
    SolidBrush ridgeFront(Color(210, 0, 0, 1));
    PointF backRidge[18] = {};
    PointF frontRidge[18] = {};
    for (int i = 0; i < 18; ++i) {
        const float x = width * i / 17.0f;
        backRidge[i] = PointF(x, height * (0.78f + static_cast<float>(Hash2(i, 0, 4300) * 0.07)));
        frontRidge[i] = PointF(x, height * (0.86f + static_cast<float>(Hash2(i, 0, 4301) * 0.05)));
    }
    PointF backPoly[20] = {};
    PointF frontPoly[20] = {};
    std::copy(backRidge, backRidge + 18, backPoly);
    backPoly[18] = PointF(width, height);
    backPoly[19] = PointF(0, height);
    std::copy(frontRidge, frontRidge + 18, frontPoly);
    frontPoly[18] = PointF(width, height);
    frontPoly[19] = PointF(0, height);
    graphics.FillPolygon(&ridgeBack, backPoly, 20);
    graphics.FillPolygon(&ridgeFront, frontPoly, 20);

    for (int i = 0; i < 230; ++i) {
        const float x = static_cast<float>(Hash2(i, 2, 4500) * width);
        const float y = static_cast<float>(Hash2(i, 3, 4501) * height * 0.58);
        const float size = static_cast<float>(0.55 + Hash2(i, 4, 4502) * 2.35);
        const bool star = Hash2(i, 5, 4503) > 0.20;
        SolidBrush point(star ? Color(185, 198, 226, 255) : Color(78, 155, 84, 40));
        graphics.FillEllipse(&point, RectF(x, y, size, size));
    }

    for (int i = 0; i < 34; ++i) {
        const float x = static_cast<float>(Hash2(i, 6, 4550) * width);
        const float y = static_cast<float>(Hash2(i, 7, 4551) * height * 0.52);
        const float size = static_cast<float>(1.7 + Hash2(i, 8, 4552) * 3.8);
        SolidBrush bright(Color(190, 210, 236, 255));
        graphics.FillEllipse(&bright, RectF(x, y, size, size));
    }

    SolidBrush defocus(Color(92, 2, 1, 4));
    graphics.FillRectangle(&defocus, skyRect);

    SolidBrush dusk(Color(82, 0, 0, 0));
    graphics.FillRectangle(&dusk, skyRect);

    SolidBrush vignette(Color(118, 0, 0, 0));
    graphics.FillRectangle(&vignette, RectF(0, 0, width, height * 0.06f));
    graphics.FillRectangle(&vignette, RectF(0, 0, width * 0.04f, height));
    graphics.FillRectangle(&vignette, RectF(width * 0.96f, 0, width * 0.04f, height));
}

void DrawHellscapeBackground(Graphics& graphics) {
    const int blurW = std::max(1, gApp.width / 6);
    const int blurH = std::max(1, gApp.height / 6);
    Bitmap skyBuffer(blurW, blurH, PixelFormat32bppPARGB);
    Graphics skyGraphics(&skyBuffer);
    skyGraphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    skyGraphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    skyGraphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    DrawHellscapeBackgroundLayer(skyGraphics, static_cast<float>(blurW), static_cast<float>(blurH));

    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.DrawImage(&skyBuffer, RectF(0, 0, static_cast<float>(gApp.width), static_cast<float>(gApp.height)));
}

void DrawScene(HDC hdc) {
    Bitmap backBuffer(gApp.width, gApp.height, PixelFormat32bppPARGB);
    Graphics graphics(&backBuffer);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    DrawHellscapeBackground(graphics);

    const int minX = 0;
    const int maxX = kWorldSize - 1;
    const int minY = 0;
    const int maxY = kWorldSize - 1;

    std::vector<RenderTile> renderTiles;
    for (int x = minX; x <= maxX; ++x) {
        for (int y = minY; y <= maxY; ++y) {
            for (int z = 0; z < kWorldSize; ++z) {
                if (!ExposedBlockAt(x, y, z)) continue;
                const Tile tile = SurfaceTileFor(x, y, z);
                renderTiles.push_back({x, y, z, tile, RealmFor(x, y)});
            }
        }
    }
    std::sort(renderTiles.begin(), renderTiles.end(), [](const RenderTile& a, const RenderTile& b) {
        const double aDepth = ViewDepth(a.x, a.y, a.z);
        const double bDepth = ViewDepth(b.x, b.y, b.z);
        if (aDepth != bDepth) return aDepth < bDepth;
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    });

    const float spriteOffsetX = static_cast<float>(-kSpriteW * 0.5 * gApp.zoom);
    const float spriteOffsetY = static_cast<float>(-ViewHalfH() * gApp.zoom);

    for (const RenderTile& renderTile : renderTiles) {
        Bitmap* image = ImageFor(renderTile.realm, renderTile.tile);
        if (!image) continue;

        const PointF pos = WorldToScreen3(renderTile.x, renderTile.y, renderTile.z);
        if (renderTile.anchoredToFeet) {
            const float drawW = static_cast<float>(image->GetWidth() * renderTile.scale * gApp.zoom);
            const float drawH = static_cast<float>(image->GetHeight() * renderTile.scale * gApp.zoom);
            DrawTerrainImage(graphics, image, RectF(pos.X - drawW * 0.5f,
                                                    pos.Y + static_cast<float>(ViewHalfH() * gApp.zoom) - drawH,
                                                    drawW, drawH));
        } else {
            const float drawW = static_cast<float>(kSpriteW * gApp.zoom);
            const float drawH = static_cast<float>(kSpriteH * gApp.zoom);
            DrawTerrainImage(graphics, image, RectF(pos.X + spriteOffsetX, pos.Y + spriteOffsetY,
                                                    drawW, drawH));
            if (!SolidBlockAt(renderTile.x, renderTile.y, renderTile.z + 1)) {
                PointF points[4] = {PointF(), PointF(), PointF(), PointF()};
                CellDiamondPoints(renderTile.x, renderTile.y, renderTile.z, points);
                Pen topPen(Color(135, 255, 170, 92), 1.0f);
                graphics.DrawPolygon(&topPen, points, 4);
            }
        }
    }

    std::vector<RenderTile> tunnelCells;
    tunnelCells.reserve(gApp.removedBlocks.size() + 1);
    for (const std::wstring& key : gApp.removedBlocks) {
        int x = 0;
        int y = 0;
        int z = 0;
        if (ParseBlockKey(key, x, y, z)) {
            tunnelCells.push_back({x, y, z, {}, L""});
        }
    }
    const Player& active = ActivePlayerConst();
    if (InWorldCube(active.x, active.y, active.z) && !SolidBlockAt(active.x, active.y, active.z)) {
        tunnelCells.push_back({active.x, active.y, active.z, {}, L""});
    }
    std::sort(tunnelCells.begin(), tunnelCells.end(), [](const RenderTile& a, const RenderTile& b) {
        const double aDepth = ViewDepth(a.x, a.y, a.z);
        const double bDepth = ViewDepth(b.x, b.y, b.z);
        if (aDepth != bDepth) return aDepth < bDepth;
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    });
    for (const RenderTile& cell : tunnelCells) {
        DrawTunnelCell(graphics, cell.x, cell.y, cell.z,
                       cell.x == active.x && cell.y == active.y && cell.z == active.z);
    }
    for (int playerIndex = 0; playerIndex < kPlayers; ++playerIndex) {
        DrawControllerCore(graphics, playerIndex);
    }

    if (gApp.playerSelected) {
        for (const MoveOption& option : gApp.moveOptions) {
            DrawOptionHighlight(graphics, option);
        }
    }

    for (int playerIndex = 0; playerIndex < kPlayers; ++playerIndex) {
        if (playerIndex == gApp.activePlayer) continue;
        for (int sphereIndex = 0; sphereIndex < kSpheresPerPlayer; ++sphereIndex) {
            DrawPlayerSphere(graphics, playerIndex, sphereIndex);
        }
    }
    for (int sphereIndex = 0; sphereIndex < kSpheresPerPlayer; ++sphereIndex) {
        if (sphereIndex == gApp.activeSphere) continue;
        DrawPlayerSphere(graphics, gApp.activePlayer, sphereIndex);
    }
    DrawPlayerSphere(graphics, gApp.activePlayer, gApp.activeSphere);

    int visibleCount = 0;
    for (const RenderTile& renderTile : renderTiles) {
        const PointF pos = WorldToScreen3(renderTile.x, renderTile.y, renderTile.z);
        Bitmap* image = ImageFor(renderTile.realm, renderTile.tile);
        const float drawW = renderTile.anchoredToFeet && image
                                ? static_cast<float>(image->GetWidth() * renderTile.scale * gApp.zoom)
                                : static_cast<float>(kSpriteW * gApp.zoom);
        const float drawH = renderTile.anchoredToFeet && image
                                ? static_cast<float>(image->GetHeight() * renderTile.scale * gApp.zoom)
                                : static_cast<float>(kSpriteH * gApp.zoom);
        const float boundsX = renderTile.anchoredToFeet ? pos.X - drawW * 0.5f : pos.X + spriteOffsetX;
        const float boundsY = renderTile.anchoredToFeet
                                  ? pos.Y + static_cast<float>(ViewHalfH() * gApp.zoom) - drawH
                                  : pos.Y + spriteOffsetY;
        const RectF bounds(boundsX, boundsY, drawW, drawH);
        if (bounds.GetRight() < 0 || bounds.X > gApp.width || bounds.GetBottom() < 0 ||
            bounds.Y > gApp.height) {
            continue;
        }
        ++visibleCount;
    }

    DrawHud(graphics, visibleCount);

    Graphics screen(hdc);
    screen.DrawImage(&backBuffer, 0, 0);
}

void Pan(double dx, double dy, double multiplier) {
    gApp.cameraX += dx * kFootprintW * gApp.zoom * multiplier;
    gApp.cameraY += dy * kFootprintH * gApp.zoom * multiplier;
    InvalidateRect(gApp.hwnd, nullptr, FALSE);
}

void CenterCameraOnPlayer() {
    const Player& player = ActivePlayerConst();
    const Vec3 view = ViewTransform(player.x, player.y, player.z);
    gApp.cameraX = (view.x - view.y) * kHalfW * gApp.zoom;
    gApp.cameraY = ((view.x + view.y) * ViewHalfH() - view.z * ViewLayerH()) * gApp.zoom;
}

void CenterCameraOnCube() {
    const double center = (kWorldSize - 1) * 0.5;
    const Vec3 view = ViewTransform(center, center, center);
    gApp.cameraX = (view.x - view.y) * kHalfW * gApp.zoom;
    gApp.cameraY = ((view.x + view.y) * ViewHalfH() - view.z * ViewLayerH()) * gApp.zoom;
}

RectF ActorBounds(int playerIndex, int sphereIndex) {
    const Player& player = gApp.players[playerIndex][sphereIndex];
    Bitmap* image = ImageFor(L"actor", {player.sprite});
    const float drawW = static_cast<float>((image ? image->GetWidth() : 18) * kActorScale * gApp.zoom);
    const float drawH = static_cast<float>((image ? image->GetHeight() : 18) * kActorScale * gApp.zoom);
    const PointF pos = WorldToScreen3(player.x, player.y, player.z);
    return RectF(pos.X - drawW * 0.5f,
                 pos.Y + static_cast<float>(ViewHalfH() * gApp.zoom) - drawH, drawW, drawH);
}

bool PointInRect(const RectF& rect, int x, int y) {
    return x >= rect.X && x <= rect.GetRight() && y >= rect.Y && y <= rect.GetBottom();
}

void BuildMoveOptions() {
    gApp.moveOptions.clear();
    if (gApp.winner >= 0) return;
    const Player& player = ActivePlayerConst();
    const struct Direction {
        int dx;
        int dy;
        int dz;
        const wchar_t* facing;
    } directions[] = {
        {0, -1, 0, L"N"},
        {1, 0, 0, L"E"},
        {0, 1, 0, L"S"},
        {-1, 0, 0, L"W"},
        {0, 0, 1, L"U"},
        {0, 0, -1, L"D"},
    };

    for (const Direction& direction : directions) {
        for (int distance = 1; distance <= 2; ++distance) {
            const int x = player.x + direction.dx * distance;
            const int y = player.y + direction.dy * distance;
            const int z = player.z + direction.dz * distance;
            if (!IsPlayableCell(x, y, z)) break;

            const bool tunnel = SolidBlockAt(x, y, z);
            int sphereOwner = -1;
            int sphereIndex = -1;
            const bool occupied = OccupyingSphereAt(x, y, z, &sphereOwner, &sphereIndex);
            if (occupied) break;

            int controllerOwner = -1;
            const bool controller = AnyControllerAt(x, y, z, &controllerOwner);
            if (controller && controllerOwner == gApp.activePlayer) break;
            if (controller && !tunnel) break;

            if (tunnel && distance > 1) break;
            gApp.moveOptions.push_back(
                {x, y, z, direction.dx, direction.dy, direction.dz,
                 tunnel ? OptionKind::Tunnel : OptionKind::Move, direction.facing});
            if (tunnel || controller) break;
        }

        for (int distance = 1; distance <= kArcLanceRange; ++distance) {
            const int x = player.x + direction.dx * distance;
            const int y = player.y + direction.dy * distance;
            const int z = player.z + direction.dz * distance;
            if (!IsPlayableCell(x, y, z)) break;
            if (SolidBlockAt(x, y, z)) break;

            int sphereOwner = -1;
            int sphereIndex = -1;
            if (OccupyingSphereAt(x, y, z, &sphereOwner, &sphereIndex)) {
                if (sphereOwner != gApp.activePlayer) {
                    gApp.moveOptions.push_back(
                        {x, y, z, direction.dx, direction.dy, direction.dz,
                         OptionKind::ArcLance, direction.facing});
                }
                break;
            }

            int controllerOwner = -1;
            if (AnyControllerAt(x, y, z, &controllerOwner)) {
                if (controllerOwner != gApp.activePlayer && ControllerRevealed(controllerOwner)) {
                    gApp.moveOptions.push_back(
                        {x, y, z, direction.dx, direction.dy, direction.dz,
                         OptionKind::ArcLance, direction.facing});
                }
                break;
            }
        }

        const int bx = player.x + direction.dx;
        const int by = player.y + direction.dy;
        const int bz = player.z + direction.dz;
        const int bx2 = player.x + direction.dx * 2;
        const int by2 = player.y + direction.dy * 2;
        const int bz2 = player.z + direction.dz * 2;
        if (SolidBlockAt(bx, by, bz) && SolidBlockAt(bx2, by2, bz2)) {
            gApp.moveOptions.push_back(
                {bx2, by2, bz2, direction.dx, direction.dy, direction.dz,
                 OptionKind::BreachCharge, direction.facing});
        }
    }
}

void SelectPlayer() {
    gApp.playerSelected = true;
    BuildMoveOptions();
    InvalidateRect(gApp.hwnd, nullptr, FALSE);
}

void ClearSelection() {
    gApp.playerSelected = false;
    gApp.moveOptions.clear();
}

void EndTurn();

void CheckVictory() {
    if (gApp.winner >= 0) return;
    for (int playerIndex = 0; playerIndex < kPlayers; ++playerIndex) {
        const int opponent = 1 - playerIndex;
        if (!gApp.controllers[playerIndex].alive) {
            SetWinner(opponent, L"enemy controller destroyed");
            return;
        }
        if (LivingSphereCount(playerIndex) == 0) {
            SetWinner(opponent, L"all enemy drones destroyed");
            return;
        }
    }
}

bool DamageEnemyAt(int x, int y, int z) {
    int sphereOwner = -1;
    int sphereIndex = -1;
    if (OccupyingSphereAt(x, y, z, &sphereOwner, &sphereIndex) && sphereOwner != gApp.activePlayer) {
        gApp.players[sphereOwner][sphereIndex].alive = false;
        return true;
    }

    int controllerOwner = -1;
    if (AnyControllerAt(x, y, z, &controllerOwner) && controllerOwner != gApp.activePlayer) {
        gApp.controllers[controllerOwner].alive = false;
        return true;
    }
    return false;
}

bool FireArcLance(const MoveOption& option) {
    if (gApp.winner >= 0) return false;
    ActivePlayer().facing = option.facing;
    if (!DamageEnemyAt(option.x, option.y, option.z)) return false;
    CheckVictory();
    EndTurn();
    InvalidateRect(gApp.hwnd, nullptr, FALSE);
    return true;
}

bool DetonateBreachCharge(const MoveOption& option) {
    if (gApp.winner >= 0) return false;
    ActivePlayer().facing = option.facing;

    const Player& player = ActivePlayerConst();
    const int firstX = player.x + option.dx;
    const int firstY = player.y + option.dy;
    const int firstZ = player.z + option.dz;

    bool changed = false;
    if (SolidBlockAt(firstX, firstY, firstZ)) {
        RemoveBlockAt(firstX, firstY, firstZ);
        changed = true;
    }
    if (SolidBlockAt(option.x, option.y, option.z)) {
        RemoveBlockAt(option.x, option.y, option.z);
        changed = true;
    }

    DamageEnemyAt(firstX, firstY, firstZ);
    DamageEnemyAt(option.x, option.y, option.z);
    CheckVictory();
    if (!changed && gApp.winner < 0) return false;
    EndTurn();
    InvalidateRect(gApp.hwnd, nullptr, FALSE);
    return true;
}

void EndTurn() {
    if (gApp.winner >= 0) {
        ClearSelection();
        return;
    }
    ++gApp.turn;
    ++gApp.actionsThisPlayer;
    if (gApp.actionsThisPlayer >= 2) {
        gApp.actionsThisPlayer = 0;
        gApp.activePlayer = 1 - gApp.activePlayer;
        gApp.activeSphere = FirstLivingSphere(gApp.activePlayer);
    }
    ClearSelection();
    CenterCameraOnPlayer();
}

bool EnterOrDig(int x, int y, int z, bool spendTurn) {
    if (gApp.winner >= 0) return false;
    if (!IsPlayableCell(x, y, z)) return false;

    int sphereOwner = -1;
    int sphereIndex = -1;
    if (OccupyingSphereAt(x, y, z, &sphereOwner, &sphereIndex)) {
        if (sphereOwner == gApp.activePlayer) return false;
        gApp.players[sphereOwner][sphereIndex].alive = false;
    }

    int controllerOwner = -1;
    if (AnyControllerAt(x, y, z, &controllerOwner)) {
        if (controllerOwner == gApp.activePlayer) return false;
        gApp.controllers[controllerOwner].alive = false;
    }

    if (SolidBlockAt(x, y, z)) {
        RemoveBlockAt(x, y, z);
    }

    Player& player = ActivePlayer();
    player.x = x;
    player.y = y;
    player.z = z;
    if (spendTurn) {
        CheckVictory();
        EndTurn();
    } else {
        ClearSelection();
    }
    return true;
}

void MovePlayer(int dx, int dy, const std::wstring& facing) {
    Player& player = ActivePlayer();
    player.facing = facing;
    const int nextX = player.x + dx;
    const int nextY = player.y + dy;
    const int nextZ = player.z;
    EnterOrDig(nextX, nextY, nextZ, true);
    CenterCameraOnPlayer();
    InvalidateRect(gApp.hwnd, nullptr, FALSE);
}

bool TryExecuteClickedOption(int screenX, int screenY) {
    for (const MoveOption& option : gApp.moveOptions) {
        if (PointInCellDiamond(screenX, screenY, option.x, option.y, option.z)) {
            if (option.kind == OptionKind::ArcLance) {
                return FireArcLance(option);
            }
            if (option.kind == OptionKind::BreachCharge) {
                return DetonateBreachCharge(option);
            }
            ActivePlayer().facing = option.facing;
            EnterOrDig(option.x, option.y, option.z, true);
            InvalidateRect(gApp.hwnd, nullptr, FALSE);
            return true;
        }
    }
    return false;
}

void ApplyViewRotation(const int rotation[9]) {
    int next[9] = {};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            next[row * 3 + col] = rotation[row * 3 + 0] * gApp.view[0 * 3 + col] +
                                  rotation[row * 3 + 1] * gApp.view[1 * 3 + col] +
                                  rotation[row * 3 + 2] * gApp.view[2 * 3 + col];
        }
    }
    std::copy(next, next + 9, gApp.view);
    ++gApp.viewTurns;
    if (gApp.playerSelected) BuildMoveOptions();
    CenterCameraOnCube();
    InvalidateRect(gApp.hwnd, nullptr, FALSE);
}

void RotateViewLeft() {
    const int rotation[9] = {0, -1, 0, 1, 0, 0, 0, 0, 1};
    ApplyViewRotation(rotation);
}

void RotateViewRight() {
    const int rotation[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1};
    ApplyViewRotation(rotation);
}

void RotateViewUp() {
    const int rotation[9] = {1, 0, 0, 0, 0, -1, 0, 1, 0};
    ApplyViewRotation(rotation);
}

void RotateViewDown() {
    const int rotation[9] = {1, 0, 0, 0, 0, 1, 0, -1, 0};
    ApplyViewRotation(rotation);
}

void HandleLeftClick(int screenX, int screenY) {
    if (gApp.winner >= 0) return;
    if (gApp.playerSelected && TryExecuteClickedOption(screenX, screenY)) return;

    for (int sphereIndex = kSpheresPerPlayer - 1; sphereIndex >= 0; --sphereIndex) {
        const Player& player = gApp.players[gApp.activePlayer][sphereIndex];
        if (!player.alive) continue;
        if (PointInRect(ActorBounds(gApp.activePlayer, sphereIndex), screenX, screenY)) {
            SetActiveSphere(sphereIndex);
            SelectPlayer();
            CenterCameraOnPlayer();
            return;
        }
    }

    ClearSelection();
    InvalidateRect(gApp.hwnd, nullptr, FALSE);
}

Player MakeSphere(int side, int index, int x, int y, int z) {
    for (int radius = 0; radius < 5; ++radius) {
        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dy = -radius; dy <= radius; ++dy) {
                const int sx = x + dx;
                const int sy = y + dy;
                if (!IsPlayableCell(sx, sy, z) || SolidBlockAt(sx, sy, z)) continue;
                return {sx, sy, z, true, L"S", side == 0 ? L"sphere-blue" : L"sphere-red"};
            }
        }
    }
    (void)index;
    return {x, y, z, true, L"S", side == 0 ? L"sphere-blue" : L"sphere-red"};
}

DroneController MakeController(int side) {
    const int baseX = side == 0 ? 7 : kWorldSize - 8;
    const int baseY = side == 0 ? kWorldSize / 2 - 2 : kWorldSize / 2 + 2;
    const int baseZ = 7;
    for (int radius = 0; radius < 10; ++radius) {
        for (int dz = -radius; dz <= radius; ++dz) {
            for (int dx = -radius; dx <= radius; ++dx) {
                for (int dy = -radius; dy <= radius; ++dy) {
                    if (std::abs(dx) + std::abs(dy) + std::abs(dz) > radius) continue;
                    const int x = baseX + dx;
                    const int y = baseY + dy;
                    const int z = baseZ + dz;
                    if (!InWorldCube(x, y, z)) continue;
                    if (!NaturalSolidBlockAt(x, y, z)) continue;
                    return {x, y, z, true};
                }
            }
        }
    }
    return {baseX, baseY, baseZ, true};
}

void ResetMatchPieces() {
    gApp.removedBlocks.clear();
    gApp.tunnelOwners.clear();
    gApp.controllers[0] = MakeController(0);
    gApp.controllers[1] = MakeController(1);
    for (int i = 0; i < kSpheresPerPlayer; ++i) {
        const int offsetY = i - 1;
        gApp.players[0][i] = MakeSphere(0, i, 5, kWorldSize / 2 + offsetY * 2, 12);
        gApp.players[1][i] = MakeSphere(1, i, kWorldSize - 6, kWorldSize / 2 + offsetY * 2, 12);
    }
    gApp.activePlayer = 0;
    gApp.activeSphere = 0;
    gApp.actionsThisPlayer = 0;
    gApp.turn = 0;
    gApp.winner = -1;
    gApp.winReason.clear();
    ClearSelection();
}

void RandomizeSeed() {
    std::mt19937 rng(static_cast<uint32_t>(GetTickCount64()));
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"seed-%08x", rng());
    gApp.seedText = buffer;
    gApp.seedHash = HashString(gApp.seedText);
    ResetMatchPieces();
    gApp.zoom = kDefaultZoom;
    CenterCameraOnCube();
    InvalidateRect(gApp.hwnd, nullptr, FALSE);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            gApp.hwnd = hwnd;
            gApp.seedHash = HashString(gApp.seedText);
            LoadImages();
            ResetMatchPieces();
            gApp.zoom = kDefaultZoom;
            CenterCameraOnCube();
            return 0;

        case WM_SIZE:
            gApp.width = std::max(1, static_cast<int>(LOWORD(lParam)));
            gApp.height = std::max(1, static_cast<int>(HIWORD(lParam)));
            if (gApp.turn == 0 && !gApp.playerSelected) {
                CenterCameraOnCube();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN:
            gApp.dragging = true;
            gApp.dragMoved = false;
            gApp.dragStart = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            gApp.dragCameraX = gApp.cameraX;
            gApp.dragCameraY = gApp.cameraY;
            SetCapture(hwnd);
            return 0;

        case WM_MOUSEMOVE:
            if (gApp.dragging) {
                const int x = GET_X_LPARAM(lParam);
                const int y = GET_Y_LPARAM(lParam);
                if (std::abs(x - gApp.dragStart.x) > 4 || std::abs(y - gApp.dragStart.y) > 4) {
                    gApp.dragMoved = true;
                }
                gApp.cameraX = gApp.dragCameraX - (x - gApp.dragStart.x);
                gApp.cameraY = gApp.dragCameraY - (y - gApp.dragStart.y);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (!gApp.dragMoved) {
                HandleLeftClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            }
            gApp.dragging = false;
            ReleaseCapture();
            return 0;

        case WM_RBUTTONDOWN:
            RotateViewRight();
            return 0;

        case WM_MBUTTONDOWN:
            RotateViewLeft();
            return 0;

        case WM_MOUSEWHEEL: {
            const double oldZoom = gApp.zoom;
            const short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            gApp.zoom = Clamp(gApp.zoom * (delta > 0 ? 1.08 : 0.92), 0.55, 5.0);
            const POINTS point = MAKEPOINTS(lParam);
            POINT client = {point.x, point.y};
            ScreenToClient(hwnd, &client);
            gApp.cameraX = (gApp.cameraX + client.x - gApp.width / 2.0) * (gApp.zoom / oldZoom) -
                           client.x + gApp.width / 2.0;
            gApp.cameraY = (gApp.cameraY + client.y - gApp.height / 2.0) * (gApp.zoom / oldZoom) -
                           client.y + gApp.height / 2.0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_KEYDOWN: {
            if (wParam == 'A') RotateViewLeft();
            if (wParam == 'D') RotateViewRight();
            if (wParam == 'W') RotateViewUp();
            if (wParam == 'S') RotateViewDown();
            if (wParam == 'C') {
                gApp.zoom = kDefaultZoom;
                CenterCameraOnCube();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (wParam == 'R') RandomizeSeed();
            if (wParam == VK_OEM_4) {
                gApp.density = Clamp(gApp.density - 0.05, 0.0, 1.0);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (wParam == VK_OEM_6) {
                gApp.density = Clamp(gApp.density + 0.05, 0.0, 1.0);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            HDC hdc = BeginPaint(hwnd, &ps);
            DrawScene(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    Gdiplus::GdiplusStartupInput startupInput;
    if (Gdiplus::GdiplusStartup(&gGdiToken, &startupInput, nullptr) != Status::Ok) {
        MessageBoxW(nullptr, L"Failed to start GDI+.", L"Infinite Iso Middle-earth", MB_ICONERROR);
        return 1;
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, kClassName, L"Infinite Iso Middle-earth", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, nullptr, instance,
                                nullptr);
    if (!hwnd) {
        Gdiplus::GdiplusShutdown(gGdiToken);
        return 1;
    }

    ShowWindow(hwnd, showCommand);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    gApp.images.clear();
    Gdiplus::GdiplusShutdown(gGdiToken);
    return 0;
}
