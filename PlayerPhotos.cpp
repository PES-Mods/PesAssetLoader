#include "Ui.hpp"
#include "FlagReplacer.hpp"
#include "PesImGui.h"
#include <intrin.h>
#include <map>
#include <vector>

// Player photos ([PLAYER_PHOTOS]): FOLDER\<player id>.png|.dds shown on some menu panels. FOLDER\background.png|.dds
// is drawn behind every photo (a bit bigger), FOLDER\no_photo.png|.dds for the players without a photo.
// A photo is drawn by a render node of its own, added right after the layout object of its panel (the anchor) with
// the same layer and key: it comes right after the panel, under everything in front of the panel (popups), and goes
// with it. The photo is placed relative to the panel's drawn meshes and fades with it.
// The menus are drawn after the 3D models, with the PS2 depth test: the photo gets the depth of its panel.
// Edit player, 3D head: the head can be moved (EDIT_HEAD_MODEL_Y): while Edit player is shown with the head camera
// (the menu camera's eye near its target; not the full body), the 3D projection is made with its center moved (in
// the camera's projection builder: the matrices are made again when the camera's dirty flag is set).
// Edit player, the list of players: the 3D player is drawn in a clip rectangle (the viewport) that cuts him at the
// sides. Its position comes from the rectangle too (PES6 moves 3D in a viewport by a "lens shift", applied only when
// the viewport isn't the whole screen), so with EDIT_LIST_FULL_3D and the head camera the game applies its rectangle
// and the viewport is then made the whole backbuffer but its last line (ApplyScissor again: the same position); the
// menus are in front of the head. With the full body its left side is the screen's (as far as a widescreen plugin
// lets a rectangle go; the full body would be drawn over the player list).
// Master League, the lists of players (search, own team): the info panel shows the selected player (the panel is
// filled by ListPanelFill(stats page, player, compared player, compare mode): the photo is shown when nothing is
// compared). The popup of a player (negotiations...) is a panel shown while the list is in mode 1, its player in the
// popup's object (+1160). PES6 shows a small 3D head there (a face of its face thumbnails: a clip rectangle of 40 x
// 70 GS pixels around the face): the photo frame (square, the place's height) is centered on it and the face's clip
// rectangle is the photo's; the photo is drawn over the face (ML_POPUP_NO_PHOTO: without a photo, the 3D face on the
// background or no_photo.png). A face is drawn at its center by a lens shift (FaceShift(center)): moved with the frame.
// 3D in a menu viewport gets a projection of its own (SetCustomProjection(matrix) in ApplyScissor: to the viewport's
// coordinates): for the face its x and y are scaled (ML_POPUP_3D_SCALE), around the viewport's center (the face's).
// Season results (the list of players): the selected player beside the header. The screen's object: +1024 the mode
// (1: the players), +1010 the first row shown, +1038 the cursor, +1039 the number of players, +1040 the season; the
// players of a season: its record (224 bytes, Records() + 224 * (season % 15)): +4 + 6 * i the player (word), the
// rows show the ones that aren't 0. League mode has the same screen (its object's fields 4 bytes further, the header:
// asset 2 of its layout file).
// The rankings (goals / assists in the league and the records room, points): a photo in every row. The rows are
// filled by name: the player's name (PlayerName(player)) is put in a text item of the row (SetText(item, format,
// ...): the item's +16 is its layout object, the row): the player of each row is taken there (both functions hooked
// at their start: every list of the rankings is filled with them).
// PES6, ML_NO_PHOTO = 3d: a player without a photo gets their 3D face in the places that show one player (the info
// panel of the lists of players, the season results, the popup): one of the game's face thumbnails (FaceAdd(player,
// center, 0, size) adds faces 0, 1, ..., FaceLoad() every frame until it returns 1 loads them all, FacePosition,
// FaceShow; FaceRemove() frees them all). The face is in front of the panel's other things (the 3D models are drawn
// first, with a nearer depth): after the background the depth of the frame is made the panel's, so what is drawn
// later in front of the panel covers or darkens the face as well (a popup's dim layer). The plugin's face is seen
// while its anchor is drawn at least half opaque
// (else hidden when it's drawn: its projection scaled to 0); it's removed when the anchor is gone, before the game adds
// faces of its own and before the object of its screen is freed (the faces are in the memory of the menus). The popup
// moves face 0: its face is added first, the plugin's after it, both loaded by the popup; when the popup is closed its
// face is kept (hidden) while the plugin's is seen, so that one isn't loaded again (the popup of the same player uses
// the kept face again).
// Places ("height, x, y" in the ini) can be changed in game in a window of PesImGui (with UI_INSPECTOR).
// No files (or no texture hooks): no photos, the rest is the same (the 3D head of Edit player, the 3D faces).
namespace PlayerPhotos {

using namespace TextureLimits;

// --- places ---
struct Place {
    char const *name;
    float height;       // GS lines
    float x, y;         // GS pixels / lines from where the place puts the photo
};

enum PlaceId { EDIT_LIST, EDIT_MENU, ML_LIST, ML_POPUP, ML_SEASON, ML_RANKING, ML_POINTS, LEAGUE_SEASON, NUM_PLACES };
constinit static Place gPlaces[NUM_PLACES] = {
    { "EDIT_LIST", 68.0f, 0.0f, 0.0f },     // on top of the info box, at its right side
    { "EDIT_MENU", 68.0f, 0.0f, 0.0f },     // on top of the info box, at its left side
    { "ML_LIST", 50.0f, -4.0f, -3.0f },     // in the info panel, bottom right
    { "ML_POPUP", 62.0f, -2.0f, 4.0f },     // in the popup, top right (PES6: on the 3D face)
    { "ML_SEASON", 68.0f, 0.0f, 0.0f },     // season results: right of the header
    { "ML_RANKING", 34.0f, -152.0f, 0.0f }, // goal / assist rankings: in every row (from its right side: left of the badge)
    { "ML_POINTS", 37.0f, -152.0f, 0.0f },  // point rankings: in every row (from its right side: left of the badge)
    { "LEAGUE_SEASON", 68.0f, 0.0f, 0.0f }, // League mode, season results: right of the header
};
constinit static float gBorder = 1.0f;
constinit static float gHeadModelY = -30.0f;   // GS lines
constinit static bool gPopupNoPhotoImage = false;  // PES6, a player without a photo: no_photo (else the 3D face)
constinit static float gPopupFaceScale = 0.8f;      // PES6 popup: the size of the 3D face

static void ReadPlace(Place &place, string const &value) {
    float height, x, y;
    int n = sscanf_s(value.c_str(), " %f , %f , %f", &height, &x, &y);
    if (n >= 1)
        place.height = std::clamp(height, 4.0f, 448.0f);
    if (n >= 3) {
        place.x = x;
        place.y = y;
    }
}

// where the photos go: a layout object (OPD file of PES5 / PES6, asset) and where the player id comes from
enum class Source { EditPlayer, ListPanel };
struct Slot {
    uint16_t file[2];
    int asset;
    Source source;
};
static Slot const kSlots[] = {
    // Edit player (the list of players, the menu of a player): the info box under the player
    { { 438, 507 }, 2, Source::EditPlayer },
    // Master League, the lists of players: the info panel on the right
    { { 629, 816 }, 0, Source::ListPanel },
};

struct Addresses {
    uintptr_t editPlayerList;   // Edit mode (as kitserver: ISEDITPLAYERLIST, ISEDITPLAYERMODE, EDITEDPLAYER)
    uintptr_t editPlayerMode;
    uintptr_t editedPlayer;
    uintptr_t buildProjection;  // (matrix, distance, scale x, y, center x, y (GS pixels), z min, max, near, far)
    uintptr_t projectionCalls[2];   // in the camera's projection builder: the GS and the D3D projections
    uintptr_t projectionDirty;  // the camera's dirty flag (byte): the matrices are made again when they are used
    uintptr_t cameraEye;        // the menu camera: eye, target (x, y, z, w)
    uintptr_t cameraTarget;
    uintptr_t setClip;          // (clip object, int16 rect[4]: x, y, width, height in GS pixels)
    uintptr_t previewClipCalls[2];  // the calls that set the clip of a 3D preview (PES6: [0] a face thumbnail)
    uintptr_t applyScissor;     // (): sets the viewport (and the matrix that keeps 3D in place) from the rectangle
    uintptr_t scissorRect;      // int left, top, right, bottom (backbuffer pixels)
    uintptr_t screenWidth, screenHeight;    // int ()
    uintptr_t list;             // Master League, the lists of players: the screen's object (pointer)
    unsigned int listMode;      // in it: 1 - the popup of a player
    uintptr_t listPanelFill;    // (stats page, player, compared player, compare mode)
    uintptr_t popup;            // the object of the popup of a player (pointer): +1160 the player (word)
    uintptr_t season;           // season results: the screen's object (pointer)
    uintptr_t leagueSeason;     // League mode, season results: the screen's object (pointer)
    uintptr_t seasonRecords;    // uint8_t *(): the records of the seasons
    uintptr_t playerName;       // char *(player): the name of a player (Master League)
    uintptr_t setText;          // (text item, format, ...): sets the text of an item of a layout object
    uint16_t files[4];          // the layout files of the season results, the goal and the point rankings, the League
                                // mode's season results
    uintptr_t faceShiftCall;    // PES6: the call of FaceShift(float center[2]) when a face thumbnail is drawn
    uintptr_t customProjectionCall;     // PES6: SetCustomProjection(matrix) in ApplyScissor
    uintptr_t faceShift;
    uintptr_t customProjection;
    // PES6: the face thumbnails
    uintptr_t faceSystem;       // its object (pointer)
    uintptr_t faceAdd, faceLoad, faceShow, faceHideFrames, facePosition, faceRemove;
    uintptr_t screenFree;       // frees the object of a screen of the Master League
    uintptr_t popupFaceAdds[2]; // the popup of a player: the return addresses of its FaceAdd (opened by the list,
                                // shown again after a dialog)
    uintptr_t popupFaceRemoveCalls[3];  // its calls of FaceRemove
};
constinit static Addresses gAddr = {};
constinit static int gVersion = 0;     // 0 PES5, 1 PES6

// --- files and textures ---
const int kBackground = -1, kNoPhoto = -2;
const int kFirstTexture = 49000;
const unsigned int kMaxPictures = 48;

static map<int, wstring> &Files() {
    static map<int, wstring> files;
    return files;
}

struct Picture {
    Texture *texture = nullptr;
    int texId = 0;
    unsigned int width = 0, height = 0;
    float u = 1.0f, v = 1.0f;   // the picture's part of the texture (from its top left corner)
    DWORD lastUse = 0;
};

// loaded pictures (a picture that can't be loaded: no texture)
static map<int, Picture> &Pictures() {
    static map<int, Picture> pictures;
    return pictures;
}

static wstring PhotosFolder() {
    wstring folder = config().PHOTOS_FOLDER;
    if (folder.size() < 2 || folder[1] != L':')
        folder = PES::GameDirPath(folder);
    return folder;
}

static void ScanFiles() {
    wstring folder = PhotosFolder();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        wstring name = fd.cFileName;
        size_t dot = name.rfind(L'.');
        if (dot == wstring::npos)
            continue;
        wstring ext = name.substr(dot), stem = name.substr(0, dot);
        bool dds = !_wcsicmp(ext.c_str(), L".dds");
        if (!dds && _wcsicmp(ext.c_str(), L".png"))
            continue;
        int key;
        if (!_wcsicmp(stem.c_str(), L"background"))
            key = kBackground;
        else if (!_wcsicmp(stem.c_str(), L"no_photo"))
            key = kNoPhoto;
        else {
            wchar_t *end;
            unsigned long id = wcstoul(stem.c_str(), &end, 10);
            if (stem.empty() || *end || id > 0xFFFF)
                continue;
            key = (int)id;
        }
        // DDS before PNG
        auto it = Files().find(key);
        if (it == Files().end() || dds)
            Files()[key] = folder + L"\\" + name;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// a free texture id; the least recently used picture goes when all are taken (not one drawn in the last second:
// its id can still be in this frame's render list)
static int FreeTextureId() {
    vector<bool> used(kMaxPictures);
    for (auto const &p : Pictures()) {
        if (p.second.texId)
            used[p.second.texId - kFirstTexture] = true;
    }
    for (unsigned int i = 0; i < kMaxPictures; i++) {
        if (!used[i])
            return kFirstTexture + (int)i;
    }
    auto oldest = Pictures().end();
    for (auto it = Pictures().begin(); it != Pictures().end(); ++it) {
        if (it->second.texture && (oldest == Pictures().end() || it->second.lastUse < oldest->second.lastUse))
            oldest = it;
    }
    if (oldest == Pictures().end() || GetTickCount() - oldest->second.lastUse < 1000)
        return 0;
    int id = oldest->second.texId;
    DeleteOwnTexture(oldest->second.texture);
    Pictures().erase(oldest);
    return id;
}

static Picture const *GetPicture(int key) {
    auto it = Pictures().find(key);
    if (it != Pictures().end()) {
        it->second.lastUse = GetTickCount();
        return it->second.texture ? &it->second : nullptr;
    }
    auto file = Files().find(key);
    if (file == Files().end())
        return nullptr;
    int texId = FreeTextureId();
    if (!texId)
        return nullptr;
    Picture picture;
    picture.texture = FlagReplacer::LoadPictureTexture(file->second, (uint32_t)texId, false, picture.width,
        picture.height);
    if (picture.texture) {
        picture.texId = texId;
        picture.u = (float)picture.width / picture.texture->width;
        picture.v = (float)picture.height / picture.texture->height;
    }
    else
        LOG(LOG_LEVEL_1, "[PlayerPhotos] can't read %S (PNG, or DDS: DXT1/DXT3/DXT5/32-bit)", file->second.c_str());
    picture.lastUse = GetTickCount();
    Picture &p = Pictures()[key] = picture;
    return p.texture ? &p : nullptr;
}

// --- drawing ---
struct PhotoDraw {
    int photo, background;      // texture ids (0: none)
    float photoU, photoV, backU, backV;     // their pictures' parts of the textures
    float left, top, right, bottom;
    float backLeft, backTop, backRight, backBottom;
    int depth;                  // GS depth: the panel's
    uint32_t color;
    bool depthTest;             // false: over the 3D models
    bool backDepthTest;         // the background
    bool panelDepth;            // the depth of the frame made the panel's after the background (the 3D face)
};

// nodes in use: one frame adds a few of them
const unsigned int kMaxDraws = 64;
constinit static Ui::RenderNode gNodes[kMaxDraws] = {};
constinit static PhotoDraw gDraws[kMaxDraws] = {};
constinit static unsigned int gNextDraw = 0;

static void __cdecl DrawPhoto(void *object) {
    auto d = (PhotoDraw const *)object;
    float z = Ui::GsDepth(d->depth);
    Ui::BeginQuads();
    if (d->background) {
        Ui::DrawQuad(d->background, d->backLeft, d->backTop, d->backRight, d->backBottom, z, d->color,
            d->backDepthTest, d->backU, d->backV);
        if (d->panelDepth)
            Ui::DrawDepth(d->background, d->backLeft, d->backTop, d->backRight, d->backBottom, z);
    }
    if (d->photo)
        Ui::DrawQuad(d->photo, d->left, d->top, d->right, d->bottom, z, d->color, d->depthTest, d->photoU, d->photoV);
    Ui::EndQuads();
}

// Master League, the lists of players: the player of the info panel
constinit static uintptr_t oListPanelFill = 0;
constinit static int gListPlayer = -1;
constinit static bool gListCompare = false;

static void __cdecl OnListPanelFill(int statsPage, int player, int comparedPlayer, int compareMode) {
    gListPlayer = player & 0xFFFF;
    gListCompare = compareMode != 0;
    CallDynGlobal(oListPanelFill, statsPage, player, comparedPlayer, compareMode);
}

static int PlayerOf(Slot const &slot) {
    switch (slot.source) {
    case Source::EditPlayer:
        if (*(uint8_t *)gAddr.editPlayerList || *(uint8_t *)gAddr.editPlayerMode)
            return *(uint16_t *)gAddr.editedPlayer;
        return -1;
    case Source::ListPanel:
        if (oListPanelFill && *(void **)gAddr.list && !gListCompare && gListPlayer > 0)
            return gListPlayer;
        return -1;
    }
    return -1;
}

// Season results: the selected player of the list of players, -1 if it isn't shown
static int SeasonPlayer(uintptr_t screen, unsigned int shift) {
    auto s = screen ? *(uint8_t const **)screen : nullptr;
    if (!s)
        return -1;
    s += shift;
    if (*(int const *)(s + 1024) != 1)
        return -1;
    auto record = CallAndReturnDynGlobal<uint8_t const *>(gAddr.seasonRecords);
    if (!record)
        return -1;
    record += 224 * (s[1040] % 15);
    unsigned int selected = s[1010] + s[1038], count = s[1039], n = 0;
    for (unsigned int i = 0; i < 32 && n < count; i++) {
        int player = *(uint16_t const *)(record + 4 + 6 * i);
        if (!player)
            continue;
        if (n == selected)
            return player;
        n++;
    }
    return -1;
}

// The rankings: the player of each row (its layout object)
static map<uintptr_t, int> &RowPlayers() {
    static map<uintptr_t, int> players;
    return players;
}

constinit static uintptr_t oPlayerName = 0, oSetText = 0;
constinit static int gNamedPlayer = -1;

static char const *__cdecl OnPlayerName(int player) {
    gNamedPlayer = player & 0xFFFF;
    return CallAndReturnDynGlobal<char const *>(oPlayerName, player);
}

// layout objects of other files that get a player's name (layout file << 8 | asset): logged once
constinit static uint32_t gNamedRows[32] = {};

// the text set right after a player's name was taken: the name of the row (rows of the rankings only)
static void __cdecl OnSetText(uint8_t const *item) {
    if (item && gNamedPlayer > 0) {
        auto row = *(uint8_t const *const *)(item + 16);
        unsigned int file = row ? Ui::LayoutFile(row) & 0xFFFF : 0;
        if (file && (file == gAddr.files[1] || file == gAddr.files[2])) {
            if (RowPlayers().size() > 256)
                RowPlayers().clear();
            RowPlayers()[(uintptr_t)row] = gNamedPlayer;
        }
        else if (file) {
            int asset = Ui::LayoutAsset(row);
            uint32_t key = (file << 8) | (asset & 0xFF);
            for (uint32_t &named : gNamedRows) {
                if (named == key)
                    break;
                if (!named) {
                    named = key;
                    LOG(LOG_LEVEL_1, "[PlayerPhotos] a player's name (%d) in layout file #%u, asset %d", gNamedPlayer,
                        file, asset);
                    break;
                }
            }
        }
    }
    gNamedPlayer = -1;
}

// SetText has variable arguments: the item is taken, then the game's function goes on with the same stack
static __declspec(naked) void SetTextStub() {
    __asm {
        mov eax, [esp + 4]
        push eax
        call OnSetText
        add esp, 4
        jmp oSetText
    }
}

// the popup of a player: its player, -1 if it isn't shown
static int PopupPlayer() {
    if (!gAddr.list)
        return -1;
    auto list = *(uint8_t const **)gAddr.list;
    auto popup = *(uint8_t const **)gAddr.popup;
    if (!list || !popup || *(int const *)(list + gAddr.listMode) != 1)
        return -1;
    int player = *(uint16_t const *)(popup + 1160);
    return player ? player : -1;
}

// --- Edit player: the 3D head moved ---
const float kHeadCameraDistance = 2000.0f;     // the head camera: the eye nearer (about 1000; full body: 3100)
constinit static DWORD gEditShown = 0;         // tick of the last frame with Edit player
constinit static DWORD gEditListShown = 0;     // tick of the last frame with the list of players
constinit static bool gFullPreview = true;     // EDIT_LIST_FULL_3D
constinit static float gModelLift = 0.0f;      // in the current projection

static float CameraDistance() {
    auto eye = (float const *)gAddr.cameraEye, target = (float const *)gAddr.cameraTarget;
    float dx = eye[0] - target[0], dy = eye[1] - target[1], dz = eye[2] - target[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static bool HeadCamera() {
    return GetTickCount() - gEditShown < 200 && CameraDistance() < kHeadCameraDistance;
}

static float WantedLift() {
    return HeadCamera() ? gHeadModelY : 0.0f;
}

static int __cdecl OnBuildProjection(void *matrix, float distance, float scaleX, float scaleY, float centerX,
    float centerY, float zMin, float zMax, float zNear, float zFar)
{
    gModelLift = WantedLift();
    return CallAndReturnDynGlobal<int>(gAddr.buildProjection, matrix, distance, scaleX, scaleY, centerX,
        centerY + gModelLift, zMin, zMax, zNear, zFar);
}

// the projection is made again (with the lift wanted now) when the lift changes
static void UpdateLift() {
    if (gAddr.projectionDirty && WantedLift() != gModelLift)
        *(uint8_t *)gAddr.projectionDirty = 1;
}

static int PopupPlayer();

// PES6, the popup of a player: its 3D face is drawn (from its lens shift to the apply of its clip)
constinit static bool gFaceScale = false;
constinit static float gActiveFaceScale = 1.0f;    // of the face being drawn
constinit static uintptr_t gFaceClipApply = 0;     // the clip object's callback (+80): applies its rectangle

static void __cdecl OnSetCustomProjection(float const *matrix) {
    if (!gFaceScale || !matrix) {
        CallDynGlobal(gAddr.customProjection, matrix);
        return;
    }
    // x and y (columns 0, 1) scaled: around the viewport's center
    float m[16];
    memcpy(m, matrix, sizeof(m));
    for (int row = 0; row < 4; row++) {
        m[4 * row] *= gActiveFaceScale;
        m[4 * row + 1] *= gActiveFaceScale;
    }
    CallDynGlobal(gAddr.customProjection, m);
}

static void __cdecl OnApplyFaceClip(void *clip) {
    CallDynGlobal(gFaceClipApply, clip);
    gFaceScale = false;
}

// the 3D player of the list of players: not cut at the sides
constinit static uintptr_t gClipApply = 0;     // the clip object's callback (+80): applies its rectangle
constinit static bool gWholeViewport = false;  // the next apply of a preview clip: then the whole backbuffer

static void __cdecl OnApplyPreviewClip(void *clip) {
    CallDynGlobal(gClipApply, clip);
    if (gWholeViewport) {
        gWholeViewport = false;
        // the whole backbuffer but the last line: still a viewport smaller than the screen, so 3D keeps the place
        // the game gives it in the rectangle
        auto rect = (int *)gAddr.scissorRect;
        rect[0] = 0;
        rect[1] = 0;
        rect[2] = CallAndReturnDynGlobal<int>(gAddr.screenWidth) - 1;
        rect[3] = CallAndReturnDynGlobal<int>(gAddr.screenHeight) - 2;
        CallDynGlobal(gAddr.applyScissor);
    }
}

// PES6, the popup of a player: its 3D face (GS pixels: the face's rectangle as the game sets it, the frame the
// plugin draws)
struct GsRect {
    float x, y, w, h;
};
constinit static GsRect gPopupFace = {};
constinit static DWORD gPopupFaceTick = 0;
constinit static GsRect gPopupFrame = {};
constinit static DWORD gPopupFrameTick = 0;

// PES6: the plugin's 3D face (a player without a photo in a place that shows one player). The faces' object: +2 * i
// the player of face i (word), +520 + 36 * i its draw data (+0 its clip object, +20 its center), +1352 the number of
// faces (byte); the index of the next face: the dword after the object's pointer.
struct OwnFace {
    int player;             // -1: none
    float center[2];        // GS, from the screen's center
    GsRect frame;           // the photo's rectangle (GS)
    float scale;
    unsigned int alpha;     // of the anchor (0 - 128)
    unsigned int uiFrame;   // when the anchor was drawn (Ui::Frame())
};
constinit static OwnFace gWantedFace = { -1 };  // by the anchor
constinit static OwnFace gOwnFace = { -1 };     // added
constinit static int gOwnFaceIndex = -1;
constinit static int gPopupFaceIndex = -1;      // the popup's face: loaded and shown by the popup
constinit static int gKeptFaceIndex = -1;       // the face of a closed popup
constinit static bool gFacesLoaded = false;     // FaceLoad has returned 1 since the faces were added
constinit static bool gFacesReused = false;     // the faces are loaded: the next FaceLoad returns 1 at once
constinit static uintptr_t oFaceAdd = 0, oFaceLoad = 0, oScreenFree = 0;

static uint8_t *Faces() {
    return *(uint8_t **)gAddr.faceSystem;
}

// the face drawn: by its center or its clip object
static int FaceOfCenter(void const *center) {
    uint8_t const *faces = Faces();
    for (int i = 0; faces && i < faces[1352]; i++) {
        if (center == faces + 540 + 36 * i)
            return i;
    }
    return -1;
}

static int FaceOfClip(void const *clip) {
    uint8_t const *faces = Faces();
    for (int i = 0; faces && i < faces[1352]; i++) {
        if (clip == *(void *const *)(faces + 520 + 36 * i))
            return i;
    }
    return -1;
}

// the anchor drawn in this or the last frame
static bool OwnFaceWanted() {
    return gWantedFace.player >= 0 && Ui::Frame() - gWantedFace.uiFrame <= 1;
}

// the plugin's face to be seen: its anchor drawn at least half opaque
static bool OwnFaceSeen() {
    return OwnFaceWanted() && gWantedFace.player == gOwnFace.player && gWantedFace.alpha >= 64;
}

static void ForgetFaces() {
    gOwnFace.player = -1;
    gOwnFaceIndex = gPopupFaceIndex = gKeptFaceIndex = -1;
    gFacesLoaded = false;
    gFacesReused = false;
}

static void RemoveFaces() {
    if (Faces())
        CallDynGlobal(gAddr.faceRemove);
    ForgetFaces();
}

static void AddOwnFace() {
    float center[4] = { gWantedFace.center[0], gWantedFace.center[1], 0.0f, 0.0f };
    gOwnFaceIndex = Faces() ? *(uint8_t const *)(gAddr.faceSystem + 4) : 0;
    CallDynGlobal(oFaceAdd, gWantedFace.player, center, 0, 1);
    gOwnFace = gWantedFace;
    gFacesLoaded = false;
}

// once a frame: the plugin's face for the anchor drawn in the last frame
static void UpdateOwnFace() {
    if (!gAddr.faceSystem)
        return;
    if (!Faces())
        ForgetFaces();      // removed by the game
    bool want = OwnFaceWanted();
    if (gOwnFaceIndex >= 0 && (!want || gWantedFace.player != gOwnFace.player)) {
        if (gPopupFaceIndex >= 0)
            return;         // removed with the popup's face (hidden till then)
        RemoveFaces();
    }
    if (!want)
        return;
    if (gOwnFaceIndex < 0) {
        if (Faces())
            return;         // the game's faces
        AddOwnFace();
    }
    gOwnFace = gWantedFace;
    float center[4] = { gOwnFace.center[0], gOwnFace.center[1], 0.0f, 0.0f };
    CallDynGlobal(gAddr.facePosition, center, gOwnFaceIndex);
    if (gPopupFaceIndex >= 0)
        return;             // loaded and shown by the popup
    CallDynGlobal(gAddr.faceHideFrames, 0);
    if (!gFacesLoaded)
        CallDynGlobal(gAddr.faceLoad);
    if (gFacesLoaded)
        CallDynGlobal(gAddr.faceShow, 1);
}

// FaceLoad: 1 when all the faces are loaded
static int __cdecl OnFaceLoad() {
    if (gFacesReused && Faces()) {
        gFacesReused = false;
        return 1;
    }
    int result = CallAndReturnDynGlobal<int>(oFaceLoad);
    if (result == 1)
        gFacesLoaded = true;
    return result;
}

// the game adds a face: the plugin's faces are removed first. The popup's face: the plugin's is added again after it,
// or the face kept for the popup of the same player is used again.
static int __cdecl OnFaceAdd(int player, float *center, int rect, int size) {
    auto from = (uintptr_t)_ReturnAddress();
    bool popup = from && (from == gAddr.popupFaceAdds[0] || from == gAddr.popupFaceAdds[1]);
    uint8_t const *faces = Faces();
    if (popup && faces && gKeptFaceIndex == 0 && gOwnFaceIndex >= 0 && *(uint16_t const *)faces == (player & 0xFFFF)) {
        gKeptFaceIndex = -1;
        gPopupFaceIndex = 0;
        gFacesReused = gFacesLoaded;
        return *(int const *)(gAddr.faceSystem + 4);
    }
    if (gOwnFaceIndex >= 0 || gKeptFaceIndex >= 0)
        RemoveFaces();
    bool first = !Faces();
    int result = CallAndReturnDynGlobal<int>(oFaceAdd, player, center, rect, size);
    if (popup && first) {
        gPopupFaceIndex = 0;
        if (OwnFaceWanted())
            AddOwnFace();
    }
    return result;
}

// the popup removes its face: kept while the plugin's is wanted
static void __cdecl OnPopupFaceRemove() {
    if (gPopupFaceIndex >= 0 && gOwnFaceIndex >= 0 && OwnFaceWanted() && gWantedFace.player == gOwnFace.player) {
        gKeptFaceIndex = gPopupFaceIndex;
        gPopupFaceIndex = -1;
        return;
    }
    RemoveFaces();
}

// the object of the screen with the plugin's face is freed: the faces first
static int __cdecl OnScreenFree(void *object) {
    if (object && (object == *(void **)gAddr.list || object == *(void **)gAddr.season
        || object == *(void **)gAddr.leagueSeason))
    {
        gWantedFace.player = -1;
        if (gOwnFaceIndex >= 0 || gKeptFaceIndex >= 0)
            RemoveFaces();
    }
    return CallAndReturnDynGlobal<int>(oScreenFree, object);
}

// a clip rectangle inside a frame (not over its border)
static void InsideFrame(GsRect const &frame, int16_t *r) {
    float left = ceilf(frame.x), top = ceilf(frame.y);
    r[0] = (int16_t)left;
    r[1] = (int16_t)top;
    r[2] = (int16_t)(floorf(frame.x + frame.w) - left);
    r[3] = (int16_t)(floorf(frame.y + frame.h) - top);
}

// the clip of a face: the plugin's inside its frame, the popup's inside the photo's rectangle
static int __cdecl OnSetFaceClip(void *clip, int16_t const *rect) {
    int16_t r[4] = { rect[0], rect[1], rect[2], rect[3] };
    int face = FaceOfClip(clip);
    if (face >= 0 && face == gOwnFaceIndex)
        InsideFrame(gOwnFace.frame, r);
    else if ((face < 0 || face != gKeptFaceIndex) && PopupPlayer() >= 0) {
        gPopupFace = { (float)r[0], (float)r[1], (float)r[2], (float)r[3] };
        gPopupFaceTick = GetTickCount();
        if (GetTickCount() - gPopupFrameTick < 200)
            InsideFrame(gPopupFrame, r);
    }
    if (gFaceScale) {
        auto &apply = *(uintptr_t *)((uint8_t *)clip + 80);
        if (apply != (uintptr_t)OnApplyFaceClip) {
            gFaceClipApply = apply;
            apply = (uintptr_t)OnApplyFaceClip;
        }
    }
    return CallAndReturnDynGlobal<int>(gAddr.setClip, clip, r);
}

static int __cdecl OnSetPreviewClip(void *clip, int16_t const *rect) {
    int16_t r[4] = { rect[0], rect[1], rect[2], rect[3] };
    gWholeViewport = false;
    if (gFullPreview && GetTickCount() - gEditListShown < 200) {
        if (HeadCamera()) {
            // the callback is called right after this
            auto &apply = *(uintptr_t *)((uint8_t *)clip + 80);
            if (apply != (uintptr_t)OnApplyPreviewClip) {
                gClipApply = apply;
                apply = (uintptr_t)OnApplyPreviewClip;
            }
            gWholeViewport = true;
        }
        else {
            r[2] = (int16_t)(r[0] + r[2]);
            r[0] = 0;
        }
    }
    return CallAndReturnDynGlobal<int>(gAddr.setClip, clip, r);
}

static bool OnBeforeSubmit(Ui::RenderNode *, int, int, float) {
    UpdateLift();
    gFaceScale = false;
    static unsigned int frame = 0;
    if (frame != Ui::Frame()) {
        frame = Ui::Frame();
        UpdateOwnFace();
    }
    return true;
}

// a face drawn, its projection scaled: the plugin's (0, hidden, while it isn't to be seen), the kept one (hidden);
// the popup's moved with its frame
static void __cdecl OnFaceShift(float const *center) {
    int face = FaceOfCenter(center);
    bool own = face >= 0 && face == gOwnFaceIndex, kept = face >= 0 && face == gKeptFaceIndex;
    if (own || kept) {
        gActiveFaceScale = own && OwnFaceSeen() ? gOwnFace.scale : 0.0f;
        gFaceScale = gAddr.customProjection != 0;
        CallDynGlobal(gAddr.faceShift, center);
        return;
    }
    if (PopupPlayer() < 0 || GetTickCount() - gPopupFrameTick > 200) {
        CallDynGlobal(gAddr.faceShift, center);
        return;
    }
    float shifted[2] = { center[0] + gPlaces[ML_POPUP].x, center[1] + gPlaces[ML_POPUP].y };
    gActiveFaceScale = gPopupFaceScale;
    gFaceScale = gAddr.customProjection != 0;
    CallDynGlobal(gAddr.faceShift, shifted);
}

// The photo of the player at the place by the box of its anchor (screen pixels); noPhoto: no_photo.png for a player
// without a photo (else nothing); depthTest false: over the 3D models
static void AddPhoto(PlaceId placeId, int player, Ui::Box const &box, unsigned int alpha, int layer, int size,
    float key, bool noPhoto = true, bool depthTest = true)
{
    Picture const *photo = GetPicture(player);
    // PES6: the 3D face for a player without a photo (the places with one player; also without pictures)
    bool ownFace = !photo && gAddr.faceSystem && !gPopupNoPhotoImage
        && (placeId == ML_LIST || placeId == ML_SEASON || placeId == LEAGUE_SEASON);
    if (!photo && !noPhoto && !ownFace)
        return;
    if (!photo && !ownFace)
        photo = GetPicture(kNoPhoto);
    Picture const *background = GetPicture(kBackground);
    if (!photo && !background && !ownFace)
        return;
    Place const *place = &gPlaces[placeId];
    float scaleY = Ui::ScreenPixelsPerGsLine(), scaleX = Ui::ScreenPixelsPerGsPixel();
    float border = gBorder * scaleY;
    float height = place->height * scaleY;
    float width = photo ? height * photo->width / photo->height : height;
    float inset = 3.0f * scaleY + border;
    PhotoDraw &d = gDraws[gNextDraw];
    d.backDepthTest = depthTest;
    d.panelDepth = false;
    bool onFace = placeId == ML_POPUP && gVersion == 1;
    if (ownFace)
        width = height;
    if (onFace) {
        // PES6: the frame (photo and border) centered on the 3D face, which is clipped to it
        if (GetTickCount() - gPopupFaceTick > 500)
            return;
        float w = width / scaleX, h = place->height;     // GS pixels / lines
        float cx = gPopupFace.x + gPopupFace.w * 0.5f + place->x, cy = gPopupFace.y + gPopupFace.h * 0.5f + place->y;
        gPopupFrame = { cx - w * 0.5f, cy - h * 0.5f, w, h };
        gPopupFrameTick = GetTickCount();
        d.left = Ui::GsXToScreen((int)lroundf((1792.0f + cx - w * 0.5f) * 16.0f));
        d.right = Ui::GsXToScreen((int)lroundf((1792.0f + cx + w * 0.5f) * 16.0f));
        d.top = Ui::GsYToScreen((int)lroundf((1824.0f + cy - h * 0.5f) * 16.0f));
        d.bottom = Ui::GsYToScreen((int)lroundf((1824.0f + cy + h * 0.5f) * 16.0f));
        // without a photo: the 3D face on the background (behind the face)
        if (!GetPicture(player) && !gPopupNoPhotoImage) {
            photo = nullptr;
            d.backDepthTest = true;
        }
    }
    else switch (placeId) {
    case EDIT_LIST:
    case EDIT_MENU:
        // on top of the info box: the list of players (the box on the left): at its right side, the menu of a
        // player (the box in the middle): at its left side
        d.bottom = box.top - border - scaleY;
        d.top = d.bottom - height;
        if (placeId == EDIT_LIST) {
            d.right = box.right - border;
            d.left = d.right - width;
        }
        else {
            d.left = box.left + border;
            d.right = d.left + width;
        }
        break;
    case ML_LIST:
        // in the panel, bottom right
        d.right = box.right - inset;
        d.left = d.right - width;
        d.bottom = box.bottom - inset;
        d.top = d.bottom - height;
        break;
    case ML_POPUP:
        // in the popup, top right
        d.right = box.right - 12.0f * scaleX;
        d.left = d.right - width;
        d.top = box.top + 12.0f * scaleY;
        d.bottom = d.top + height;
        break;
    case ML_SEASON:
    case LEAGUE_SEASON:
        // right of the header
        d.left = box.right + 6.0f * scaleX + border;
        d.right = d.left + width;
        d.top = box.top + border;
        d.bottom = d.top + height;
        break;
    case ML_RANKING:
    case ML_POINTS:
        // in the row, at its right side
        d.right = box.right - inset;
        d.left = d.right - width;
        d.top = (box.top + box.bottom - height) * 0.5f;
        d.bottom = d.top + height;
        break;
    default:
        return;
    }
    float dx = onFace ? 0.0f : place->x * scaleX, dy = onFace ? 0.0f : place->y * scaleY;
    d.left += dx;
    d.right += dx;
    d.top += dy;
    d.bottom += dy;
    d.backLeft = d.left - border;
    d.backTop = d.top - border;
    d.backRight = d.right + border;
    d.backBottom = d.bottom + border;
    if (ownFace) {
        // the background behind the face; the face at the photo's place (GS, from the screen's center)
        photo = nullptr;
        d.backDepthTest = true;
        d.panelDepth = true;
        float x0 = Ui::GsXToScreen(1792 * 16), y0 = Ui::GsYToScreen(1824 * 16);
        OwnFace &f = gWantedFace;
        f.player = player;
        f.frame = { (d.left - x0) / scaleX, (d.top - y0) / scaleY, (d.right - d.left) / scaleX,
            (d.bottom - d.top) / scaleY };
        f.center[0] = f.frame.x + f.frame.w * 0.5f - 256.0f;
        f.center[1] = f.frame.y + f.frame.h * 0.5f - 224.0f;
        f.scale = gPopupFaceScale * place->height / gPlaces[ML_POPUP].height;
        f.alpha = alpha;
        f.uiFrame = Ui::Frame();
    }
    d.photo = photo ? photo->texId : 0;
    d.background = background ? background->texId : 0;
    d.photoU = photo ? photo->u : 1.0f;
    d.photoV = photo ? photo->v : 1.0f;
    d.backU = background ? background->u : 1.0f;
    d.backV = background ? background->v : 1.0f;
    d.depth = box.maxZ;
    d.color = (alpha << 24) | 0x808080;
    d.depthTest = depthTest;
    Ui::RenderNode &node = gNodes[gNextDraw];
    node = {};
    node.object = &d;
    node.callback = DrawPhoto;
    gNextDraw = (gNextDraw + 1) % kMaxDraws;
    Ui::Submit(&node, layer, size, key);
}

static void OnAfterSubmit(Ui::RenderNode *node, int layer, int size, float key) {
    auto object = (uint8_t const *)node->object;
    Ui::Box box;
    // the popup of a player (PES6: over its 3D head, only a real photo)
    if (Ui::IsPanel(node)) {
        int player = PopupPlayer();
        if (player >= 0 && Ui::PanelBounds(object, box))
            AddPhoto(ML_POPUP, player, box, Ui::PanelAlpha(object), layer, size, key, true, gVersion == 0);
        return;
    }
    if (!Ui::IsLayoutObject(node))
        return;
    unsigned int file = Ui::LayoutFile(object) & 0xFFFF;
    int asset = Ui::LayoutAsset(object);
    // season results: the header; the rankings: the rows
    if (gAddr.season && file == gAddr.files[0] && asset == 0) {
        int player = SeasonPlayer(gAddr.season, 0);
        if (player >= 0 && Ui::LayoutBounds(object, box))
            AddPhoto(ML_SEASON, player, box, Ui::LayoutAlpha(object), layer, size, key);
        return;
    }
    if (gAddr.leagueSeason && file == gAddr.files[3] && asset == 2) {
        int player = SeasonPlayer(gAddr.leagueSeason, 4);
        if (player >= 0 && Ui::LayoutBounds(object, box))
            AddPhoto(LEAGUE_SEASON, player, box, Ui::LayoutAlpha(object), layer, size, key);
        return;
    }
    if (oSetText && (file == gAddr.files[1] || file == gAddr.files[2])) {
        auto row = RowPlayers().find((uintptr_t)object);
        if (row != RowPlayers().end() && Ui::LayoutBounds(object, box))
            AddPhoto(file == gAddr.files[1] ? ML_RANKING : ML_POINTS, row->second, box, Ui::LayoutAlpha(object), layer,
                size, key);
        return;
    }
    for (Slot const &slot : kSlots) {
        if (slot.file[gVersion] != file || slot.asset != asset)
            continue;
        int player = PlayerOf(slot);
        if (player < 0 || !Ui::LayoutBounds(object, box))
            continue;
        PlaceId place = ML_LIST;
        if (slot.source == Source::EditPlayer) {
            bool list = (box.left + box.right) / 2 < Ui::ScreenCenterX();
            place = list ? EDIT_LIST : EDIT_MENU;
            gEditShown = GetTickCount();
            if (list)
                gEditListShown = gEditShown;
            UpdateLift();
        }
        AddPhoto(place, player, box, Ui::LayoutAlpha(object), layer, size, key);
    }
}

// --- the places window (with UI_INSPECTOR) ---
static void OnCreate() {
    IMGUI_SYNC
    ImGuiCheckVersion();
}

static void OnDestroy() {
    IMGUI_SYNC
}

static string PlaceText(Place const &p) {
    char buf[96];
    sprintf_s(buf, "%s = %g, %g, %g", p.name, p.height, p.x, p.y);
    return buf;
}

// a slider with - / + buttons (steps of 1)
static void Slider(char const *label, float &value, float minValue, float maxValue) {
    ImGui::PushID(label);
    float button = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 2.0f * (button + ImGui::GetStyle().ItemInnerSpacing.x)
        - ImGui::CalcTextSize("Head camera: 3D Y").x - ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SliderFloat("##slider", &value, minValue, maxValue, "%.0f");
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    if (ImGui::Button("-", ImVec2(button, button)))
        value = max(minValue, floorf(value + 0.5f) - 1.0f);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    if (ImGui::Button("+", ImVec2(button, button)))
        value = min(maxValue, floorf(value + 0.5f) + 1.0f);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);
    ImGui::PopID();
}

static void OnRender() {
    IMGUI_SYNC
    ImGui::SetNextWindowSize(ImVec2(640, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("PesAssetLoader - Player photos")) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("Height, x, y: GS lines/pixels (the screen is 512 x 448)");
    for (Place &p : gPlaces) {
        ImGui::PushID(p.name);
        ImGui::SeparatorText(p.name);
        Slider("Height", p.height, 4.0f, 224.0f);
        Slider("X", p.x, -512.0f, 512.0f);
        Slider("Y", p.y, -448.0f, 448.0f);
        if (&p == &gPlaces[ML_POPUP] && gVersion == 1) {
            ImGui::Checkbox("Players without a photo: no_photo (else the 3D face)", &gPopupNoPhotoImage);
            float percent = gPopupFaceScale * 100.0f;
            Slider("3D face size %", percent, 30.0f, 150.0f);
            gPopupFaceScale = percent / 100.0f;
        }
        ImGui::PopID();
    }
    if (gAddr.projectionDirty) {
        ImGui::SeparatorText("Edit player: the 3D head");
        Slider("Head camera: 3D Y", gHeadModelY, -224.0f, 224.0f);
        ImGui::Checkbox("List of players: the 3D player not cut at the sides", &gFullPreview);
        ImGui::TextDisabled("camera eye - target: %.0f (head camera below %.0f): %s", CameraDistance(),
            kHeadCameraDistance, HeadCamera() ? "head" : "-");
    }
    Slider("Border", gBorder, 0.0f, 16.0f);
    if (ImGui::Button("Copy ini lines")) {
        string text;
        for (Place const &p : gPlaces)
            text += PlaceText(p) + "\n";
        char buf[128];
        sprintf_s(buf, "BORDER = %g\nEDIT_HEAD_MODEL_Y = %g\nEDIT_LIST_FULL_3D = %d\nML_NO_PHOTO = %s\n"
            "ML_POPUP_3D_SCALE = %.2f\n", gBorder, gHeadModelY, gFullPreview ? 1 : 0,
            gPopupNoPhotoImage ? "no_photo" : "3d", gPopupFaceScale);
        text += buf;
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::End();
}

void Install(PES::Version v) {
    if (!config().PLAYER_PHOTOS)
        return;
    switch (v.id()) {
    case ID_PES5:
        gVersion = 0;
        gAddr = { 0x38F9AE0, 0x38F9AE4, 0x3937218, 0x428670, { 0x42DE18, 0x42DE44 }, 0xE97FD0, 0xFE5FC0, 0xFE5FD0,
            0xACA4E0, { 0x9057C8, 0 }, 0x42B270, 0xF77880, 0x412520, 0x412530, 0x3A07A70, 2792, 0xAAA1F0, 0x3A07BA4,
            0x3A07A0C, 0x3A07A3C, 0x9E7980, 0xA0F560, 0xA0DBF0, { 594, 582, 611, 605 } };
        break;
    case ID_PES6:
        gVersion = 1;
        gAddr = { 0x1108534, 0x11084E8, 0x112E24A, 0x409B80, { 0x40CEA6, 0x40CEC8 }, 0xF84330, 0x1030F20,
            0x1030F30, 0x8CE390, { 0x6D1BA5, 0xB0E884 }, 0x40D490, 0xF863A8, 0x405960, 0x405970, 0x10C8258, 3112,
            0x7001C0, 0x10C8AC0, 0x10C8A9C, 0x10C8AD8, 0x676160, 0x6CA320, 0x6BEAD0,
            { 688, 675, 797, 775 }, 0x6D1CA2, 0x40D712,
            0, 0, 0x10C8904, 0x6D1F30, 0x6D2110, 0x6D1970, 0x6D1A00, 0x6D1740, 0x6D1FD0, 0x6667D0,
            { 0x6F8ABA, 0x749534 }, { 0x74936E, 0x74942E, 0x749459 } };
        break;
    default:
        return;
    }
    ScanFiles();
    if (!Files().empty() && !HooksInstalled()) {
        LOG(LOG_LEVEL_1, "[PlayerPhotos] the texture hooks are not installed, photos are not shown");
        Files().clear();
    }
    if (!Ui::Install(v))
        return;
    gBorder = (float)std::clamp(config().PHOTOS_BORDER, 0, 64);
    ReadPlace(gPlaces[EDIT_LIST], config().PHOTOS_EDIT_LIST);
    ReadPlace(gPlaces[EDIT_MENU], config().PHOTOS_EDIT_MENU);
    ReadPlace(gPlaces[ML_LIST], config().PHOTOS_ML_LIST);
    ReadPlace(gPlaces[ML_POPUP], config().PHOTOS_ML_POPUP);
    ReadPlace(gPlaces[ML_SEASON], config().PHOTOS_ML_SEASON);
    ReadPlace(gPlaces[ML_RANKING], config().PHOTOS_ML_RANKING);
    ReadPlace(gPlaces[ML_POINTS], config().PHOTOS_ML_POINTS);
    ReadPlace(gPlaces[LEAGUE_SEASON], config().PHOTOS_LEAGUE_SEASON);
    // the rankings: the player of every row. PlayerName: push ecx; push esi; mov esi, [esp+0Ch]; SetText: push edi;
    // mov edi, [esp+8]; test edi, edi
    oPlayerName = MakeTrampoline(gAddr.playerName, "51 56 8B 74 24 0C");
    uintptr_t setText = MakeTrampoline(gAddr.setText, "57 8B 7C 24 08 85 FF");
    if (oPlayerName && setText) {
        oSetText = setText;
        patch::RedirectJump(gAddr.playerName, OnPlayerName);
        patch::RedirectJump(gAddr.setText, SetTextStub);
    }
    else
        LOG(LOG_LEVEL_1, "[PlayerPhotos] unexpected code at %X / %X, no photos in the rankings", gAddr.playerName,
            gAddr.setText);
    string noPhoto = config().PHOTOS_ML_NO_PHOTO.empty() ? config().PHOTOS_ML_POPUP_NO_PHOTO : config().PHOTOS_ML_NO_PHOTO;
    gPopupNoPhotoImage = !_stricmp(noPhoto.c_str(), "no_photo");
    try {
        gPopupFaceScale = std::clamp(std::stof(config().PHOTOS_ML_POPUP_3D_SCALE), 0.1f, 4.0f);
    }
    catch (...) {
        gPopupFaceScale = 0.8f;
    }
    // PES6, the plugin's 3D faces: FaceAdd: mov edx, [faces]; test edx, edx; FaceLoad: sub esp, 10h; push edi;
    // mov edi, [faces]; the screen free: push esi; jmp [...]; the popup's calls of FaceRemove
    if (gAddr.faceSystem) {
        oFaceAdd = MakeTrampoline(gAddr.faceAdd, "8B 15 04 89 0C 01 85 D2");
        oFaceLoad = MakeTrampoline(gAddr.faceLoad, "83 EC 10 57 8B 3D 04 89 0C 01");
        oScreenFree = MakeTrampoline(gAddr.screenFree, "56 FF 25 20 59 46 04");
        bool popupCalls = true;
        for (uintptr_t call : gAddr.popupFaceRemoveCalls) {
            popupCalls = popupCalls && patch::GetBytes(call, 1) == "E8"
                && injector::GetBranchDestination(call) == gAddr.faceRemove;
        }
        if (oFaceAdd && oFaceLoad && oScreenFree && popupCalls) {
            patch::RedirectJump(gAddr.faceAdd, OnFaceAdd);
            patch::RedirectJump(gAddr.faceLoad, OnFaceLoad);
            patch::RedirectJump(gAddr.screenFree, OnScreenFree);
            for (uintptr_t call : gAddr.popupFaceRemoveCalls)
                patch::RedirectCall(call, OnPopupFaceRemove);
        }
        else {
            gAddr.faceSystem = 0;
            LOG(LOG_LEVEL_1, "[PlayerPhotos] unexpected code at %X / %X / %X / %X, no 3D faces for players without a "
                "photo", gAddr.faceAdd, gAddr.faceLoad, gAddr.screenFree, gAddr.popupFaceRemoveCalls[0]);
        }
    }
    // PES6, the popup's 3D face: moved with the frame, scaled (only with the projection hooks)
    if (gAddr.faceShiftCall && gAddr.projectionDirty) {
        if (patch::GetBytes(gAddr.faceShiftCall, 1) == "E8") {
            gAddr.faceShift = injector::GetBranchDestination(gAddr.faceShiftCall);
            patch::RedirectCall(gAddr.faceShiftCall, OnFaceShift);
        }
        if (patch::GetBytes(gAddr.customProjectionCall, 1) == "E8") {
            gAddr.customProjection = injector::GetBranchDestination(gAddr.customProjectionCall);
            patch::RedirectCall(gAddr.customProjectionCall, OnSetCustomProjection);
        }
        else
            LOG(LOG_LEVEL_1, "[PlayerPhotos] unexpected code at %X, the 3D face isn't moved", gAddr.faceShiftCall);
    }
    // Master League, the lists of players: sub esp, 24h; push esi; call (the list's object)
    oListPanelFill = MakeTrampolineWithCall(gAddr.listPanelFill, "83 EC 24 56");
    if (oListPanelFill)
        patch::RedirectJump(gAddr.listPanelFill, OnListPanelFill);
    else
        LOG(LOG_LEVEL_1, "[PlayerPhotos] unexpected code at %X, no photos in the lists of players", gAddr.listPanelFill);
    gHeadModelY = (float)std::clamp(config().PHOTOS_EDIT_HEAD_MODEL_Y, -224, 224);
    gFullPreview = config().PHOTOS_EDIT_LIST_FULL_3D;
    Ui::AddAfterSubmit(OnAfterSubmit);
    // the 3D head moved
    bool projection = true;
    for (uintptr_t call : gAddr.projectionCalls) {
        projection = projection && patch::GetBytes(call, 1) == "E8"
            && injector::GetBranchDestination(call) == gAddr.buildProjection;
    }
    if (projection) {
        for (uintptr_t call : gAddr.projectionCalls)
            patch::RedirectCall(call, OnBuildProjection);
        Ui::AddBeforeSubmit(OnBeforeSubmit);
    }
    else {
        gAddr.projectionDirty = 0;
        LOG(LOG_LEVEL_1, "[PlayerPhotos] unexpected code at %X, the 3D head is not moved", gAddr.projectionCalls[0]);
    }
    // the 3D player of the list of players not cut at the sides
    for (uintptr_t call : gAddr.previewClipCalls) {
        if (!call)
            continue;
        if (patch::GetBytes(call, 1) == "E8" && injector::GetBranchDestination(call) == gAddr.setClip)
            patch::RedirectCall(call, gVersion == 1 && call == gAddr.previewClipCalls[0] ? (void *)OnSetFaceClip
                : (void *)OnSetPreviewClip);
        else
            LOG(LOG_LEVEL_1, "[PlayerPhotos] unexpected code at %X, the 3D player is cut at the sides", call);
    }
    LOG(LOG_LEVEL_1, "[PlayerPhotos] %u files in %S", (unsigned int)Files().size(), PhotosFolder().c_str());
    // the places window
    if (config().UI_INSPECTOR && GetFileAttributesW(PES::GameDirPath(L"plugins\\PesImGui.dll").c_str())
        != INVALID_FILE_ATTRIBUTES && ImGuiEnsureLoaded(false))
    {
        ImGuiOnCreate(OnCreate);
        ImGuiOnRender(OnRender);
        ImGuiOnDestroy(OnDestroy);
        ImGuiEnableMouseCursor();
    }
}

}

void InitPlayerPhotos(PES::Version v) {
    PlayerPhotos::Install(v);
}
