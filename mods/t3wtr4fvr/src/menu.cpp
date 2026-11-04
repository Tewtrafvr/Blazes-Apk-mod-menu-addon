// t3wtr4fvr: the Blaze starter menu with a rounded-corner panel (a rectangle with round sides).
// Based on the starter menu: a hand menu in the style of ii's Stupid Menu (Gorilla Tag).
// The layout idea comes from ii's menu; the code is new.
//   Hold Y (left hand)      -> menu on your left hand, tipped like ii's; turn your palm to you
//                              (B and the right hand with "Right Hand Menu")
//   Touch a button with your other index finger to press it
//   < and > on the sides    -> previous / next page
//   Pages: Movement Mods, Rig Mods, Items, Settings, Enabled Mods
// Movement only changes your own game. Rig mods (dances) and Items are seen by the room,
// within the limits in AGENTS.md ("Things others see"). Read docs/GUIDE.md before adding.
#include "ymsdk.hpp"
#include <cmath>
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <dirent.h>
#include <strings.h>

using namespace BNM;
using namespace BNM::Structures::Unity;
using BNM::IL2CPP::Il2CppObject;

YEEPS_MOD("t3wtr4fvr")

// ---- small helpers ---------------------------------------------------------

static std::string TypeName(const BNM::IL2CPP::Il2CppType *t) {
    Class c(t);
    if (!c.IsValid() || !c.GetClass() || !c.GetClass()->name) return "?";
    auto *k = c.GetClass();
    return (k->namespaze && k->namespaze[0]) ? std::string(k->namespaze) + "." + k->name : k->name;
}
// Pick an overload by its exact parameter types, e.g. GetComponent(System.Type).
static MethodBase Overload(Class c, const char *name, std::vector<std::string> types) {
    for (auto &m : c.GetMethods(false)) {
        auto *mi = m.GetInfo();
        if (!mi || !mi->name || strcmp(mi->name, name) != 0 || mi->parameters_count != types.size()) continue;
        bool ok = true;
        for (size_t i = 0; i < types.size() && ok; i++) ok = TypeName(mi->parameters[i]) == types[i];
        if (ok) return m;
    }
    return {};
}

// Unity objects we keep between frames must be held by a GC handle, or the
// garbage collector can free them under us.
static void *(*p_handleNew)(void *, bool);
static void *(*p_handleTarget)(void *);
static void (*p_handleFree)(void *);
struct Ref {
    void *h = nullptr;
    void set(Il2CppObject *o) { clear(); if (o && p_handleNew) h = p_handleNew(o, false); }
    Il2CppObject *get() const { return h && p_handleTarget ? (Il2CppObject *)p_handleTarget(h) : nullptr; }
    void clear() { if (h && p_handleFree) p_handleFree(h); h = nullptr; }
};

static Color Lerp(Color a, Color b, float t) {
    return Color(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1);
}
static Color Rainbow(float h) {             // hue 0..1 -> bright color
    h = h - floorf(h);
    float r = fabsf(h * 6 - 3) - 1, g = 2 - fabsf(h * 6 - 2), b = 2 - fabsf(h * 6 - 4);
    auto c = [](float v) { return v < 0 ? 0.f : v > 1 ? 1.f : v; };
    return Color(c(r), c(g), c(b), 1);
}

// ---- game lookups, resolved once -------------------------------------------

struct XRDevice { uint64_t id; bool initialized; };
static struct {
    bool ok = false;
    Method<Il2CppObject *> createPrimitive, goTransform, goGetComponent, goAddComponent, compTransform, compGameObject;
    Method<void> goSetActive, destroy, trSetParent, trSetPosition, trSetRotation, trSetLocalPosition,
                 trSetLocalRotation, trSetLocalScale, rbSetVelocity, matSetColor, matSetShader,
                 textSetFont, textSetText, textSetFontSize, textSetAlignment, graphicSetColor,
                 canvasSetRenderMode, rtSetSizeDelta, textSetFontStyle, textSetBestFit, textSetMinSize,
                 textSetMaxSize, teleportTo, rbAddForce, rbSetUseGravity, colliderSetEnabled,
                 lrSetPositionCount, lrSetPosition, lrSetStartWidth, lrSetEndWidth, lrSetUseWorldSpace;
    Method<Quaternion> trGetRotation;
    Method<Vector3> trGetPosition, trGetForward, trGetRight, trGetUp, trInverseTransformPoint, trTransformPoint, rbGetVelocity;
    Method<bool> alive, tryGetBool, haptic, raycast;
    Method<XRDevice> deviceAtNode;
    Method<Il2CppObject *> trGetParent, trGetRoot, getComponentsInChildren, netLocal;
    Method<void> pvRpc;
    BNM::IL2CPP::MethodInfo *rpcCreateItem = nullptr;     // only read for its parameter types
    BNM::IL2CPP::Il2CppClass *objectClass = nullptr, *stringClass = nullptr;
    BNM::MonoType *tPhotonView = nullptr, *tItem = nullptr;
    Method<BNM::Structures::Mono::String *> itemKey, itemId;
    Method<bool> itemHeld;
    Method<void> itemDrop, setHeldItem;
    void *(*arrayNew)(void *, uintptr_t) = nullptr;
    void *(*valueBox)(void *, void *) = nullptr;
    Method<bool> behaviourGetEnabled;
    Method<void> behaviourSetEnabled;
    std::vector<BNM::MonoType *> trackerTypes;
    Method<Il2CppObject *> rendererGetMaterial, shaderFind, findAll, findScene, findSceneByType, builtinResource;
    Field<Il2CppObject *> leftHand, rightHand, headTransform, bodyCollider, headCollider;
    Field<float> maxJumpSpeed, jumpMultiplier;
    Method<Vector3> plGetVelocity;
    Method<void> plSetVelocity, plNoGravityFor;
    Field<int> locomotionLayers, collisionLayers;
    Method<int> goGetLayer;
    Method<bool> colliderIsTrigger, behaviourGetEnabledCollider;
    Method<void> ignoreLayers;            // a LayerMask: which layers your hands can climb on
    Method<void> goSetLayer;
    BNM::MonoType *tRigidbody = nullptr, *tCollider = nullptr, *tCanvas = nullptr, *tText = nullptr,
                  *tFont = nullptr, *tRenderer = nullptr, *tMeshCollider = nullptr, *tLine = nullptr;
    Ref shader, font, usagePrimary, usageSecondary, usageGrip, usageTrigger;
} G;

static bool Lookups() {
    if (G.ok) return true;
    auto R = ym::api()->resolve;
    p_handleNew = (decltype(p_handleNew))R("il2cpp_gchandle_new");
    p_handleTarget = (decltype(p_handleTarget))R("il2cpp_gchandle_get_target");
    p_handleFree = (decltype(p_handleFree))R("il2cpp_gchandle_free");
    Class go("UnityEngine", "GameObject"), tr("UnityEngine", "Transform"), obj("UnityEngine", "Object"),
          rb("UnityEngine", "Rigidbody"), mat("UnityEngine", "Material"), rend("UnityEngine", "Renderer"),
          sh("UnityEngine", "Shader"), res("UnityEngine", "Resources"), text("UnityEngine.UI", "Text"),
          graphic("UnityEngine.UI", "Graphic"), canvas("UnityEngine", "Canvas"), rt("UnityEngine", "RectTransform"),
          dev("UnityEngine.XR", "InputDevices"), player("", "MasterPlayer", Image("Assembly-CSharp.dll"));
    if (!go || !tr || !player || !dev) { ym::log("starter-menu: game classes missing"); return false; }
    G.createPrimitive = go.GetMethod("CreatePrimitive", 1);
    G.goTransform = go.GetMethod("get_transform", 0);
    G.goGetComponent = Overload(go, "GetComponent", {"System.Type"});
    G.goAddComponent = Overload(go, "AddComponent", {"System.Type"});
    G.goSetActive = go.GetMethod("SetActive", 1);
    Class comp("UnityEngine", "Component");
    G.compTransform = comp.GetMethod("get_transform", 0);
    G.compGameObject = comp.GetMethod("get_gameObject", 0);
    G.destroy = Overload(obj, "Destroy", {"UnityEngine.Object"});
    G.alive = obj.GetMethod("op_Implicit", 1);
    G.trSetParent = Overload(tr, "SetParent", {"UnityEngine.Transform", "System.Boolean"});
    G.trSetPosition = tr.GetMethod("set_position", 1);
    G.trSetRotation = tr.GetMethod("set_rotation", 1);
    G.trSetLocalPosition = tr.GetMethod("set_localPosition", 1);
    G.trSetLocalRotation = tr.GetMethod("set_localRotation", 1);
    G.trSetLocalScale = tr.GetMethod("set_localScale", 1);
    G.trGetPosition = tr.GetMethod("get_position", 0);
    G.trGetRotation = tr.GetMethod("get_rotation", 0);
    G.trGetForward = tr.GetMethod("get_forward", 0);
    G.trGetRight = tr.GetMethod("get_right", 0);
    G.trGetUp = tr.GetMethod("get_up", 0);
    G.trInverseTransformPoint = Overload(tr, "InverseTransformPoint", {"UnityEngine.Vector3"});
    G.trTransformPoint = Overload(tr, "TransformPoint", {"UnityEngine.Vector3"});
    G.rbSetVelocity = rb.GetMethod("set_velocity", 1);
    G.rbGetVelocity = rb.GetMethod("get_velocity", 0);
    G.rendererGetMaterial = rend.GetMethod("get_material", 0);
    G.matSetColor = Overload(mat, "set_color", {"UnityEngine.Color"});
    G.matSetShader = mat.GetMethod("set_shader", 1);
    G.shaderFind = sh.GetMethod("Find", 1);
    G.findAll = Overload(res, "FindObjectsOfTypeAll", {"System.Type"});
    G.textSetFont = text.GetMethod("set_font", 1);
    G.textSetFontStyle = text.GetMethod("set_fontStyle", 1);
    G.textSetBestFit = text.GetMethod("set_resizeTextForBestFit", 1);
    G.textSetMinSize = text.GetMethod("set_resizeTextMinSize", 1);
    G.textSetMaxSize = text.GetMethod("set_resizeTextMaxSize", 1);
    G.textSetText = text.GetMethod("set_text", 1);
    G.textSetFontSize = text.GetMethod("set_fontSize", 1);
    G.textSetAlignment = text.GetMethod("set_alignment", 1);
    G.graphicSetColor = graphic.GetMethod("set_color", 1);
    G.canvasSetRenderMode = canvas.GetMethod("set_renderMode", 1);
    G.rtSetSizeDelta = rt.GetMethod("set_sizeDelta", 1);
    G.deviceAtNode = dev.GetMethod("GetDeviceAtXRNode", 1);
    G.tryGetBool = dev.GetMethod("TryGetFeatureValue_bool", 3);
    G.haptic = dev.GetMethod("SendHapticImpulse", 4);    // optional: a small buzz on press
    G.leftHand = player.GetField("leftHandTransform");
    G.rightHand = player.GetField("rightHandTransform");
    G.headTransform = player.GetField("headTransform");
    G.bodyCollider = player.GetField("bodyCollider");
    G.headCollider = player.GetField("headCollider");
    G.maxJumpSpeed = player.GetField("maxJumpSpeed");
    G.locomotionLayers = player.GetField("locomotionEnabledLayers");
    G.collisionLayers = player.GetField("collisionEnabledLayers");
    G.goGetLayer = go.GetMethod("get_layer", 0);
    G.ignoreLayers = Class("UnityEngine", "Physics").GetMethod("IgnoreLayerCollision", 3);
    // Yeeps moves your body through these, not through a Rigidbody on the player object
    G.plGetVelocity = player.GetMethod("GetVelocity", 0);
    G.plSetVelocity = player.GetMethod("ApplySetVelocityNoHaptic", 1);
    if (!G.plSetVelocity.IsValid()) G.plSetVelocity = player.GetMethod("ApplySetVelocity", 1);
    G.plNoGravityFor = player.GetMethod("ApplyNoGravityDuration", 1);
    ym::log("starter-menu: velocity %d/%d, no-gravity %d", G.plGetVelocity.IsValid(), G.plSetVelocity.IsValid(),
            G.plNoGravityFor.IsValid());
    G.goSetLayer = go.GetMethod("set_layer", 1);
    G.jumpMultiplier = player.GetField("jumpMultiplier");
    G.teleportTo = player.GetMethod("TeleportTo", 1);
    G.rbAddForce = Overload(rb, "AddForce", {"UnityEngine.Vector3", "UnityEngine.ForceMode"});
    G.rbSetUseGravity = rb.GetMethod("set_useGravity", 1);
    G.colliderSetEnabled = Class("UnityEngine", "Collider").GetMethod("set_enabled", 1);
    G.behaviourGetEnabledCollider = Class("UnityEngine", "Collider").GetMethod("get_enabled", 0);
    G.colliderIsTrigger = Class("UnityEngine", "Collider").GetMethod("get_isTrigger", 0);
    G.raycast = Overload(Class("UnityEngine", "Physics"), "Raycast",
                         {"UnityEngine.Vector3", "UnityEngine.Vector3", "UnityEngine.RaycastHit", "System.Single", "System.Int32"});
    G.findScene = Overload(obj, "FindObjectsOfType", {"System.Type"});      // only when noclip starts
    G.findSceneByType = Overload(obj, "FindObjectsByType", {"System.Type", "UnityEngine.FindObjectsSortMode"});
    G.builtinResource = Overload(res, "GetBuiltinResource", {"System.Type", "System.String"});
    Class line("UnityEngine", "LineRenderer");
    G.tLine = line.GetMonoType();
    G.lrSetPositionCount = line.GetMethod("set_positionCount", 1);
    G.lrSetPosition = line.GetMethod("SetPosition", 2);
    G.lrSetStartWidth = line.GetMethod("set_startWidth", 1);
    G.lrSetEndWidth = line.GetMethod("set_endWidth", 1);
    G.lrSetUseWorldSpace = line.GetMethod("set_useWorldSpace", 1);
    G.tMeshCollider = Class("UnityEngine", "MeshCollider").GetMonoType();
    G.trGetParent = tr.GetMethod("get_parent", 0);
    G.trGetRoot = tr.GetMethod("get_root", 0);
    G.getComponentsInChildren = Overload(go, "GetComponentsInChildren", {"System.Type", "System.Boolean"});
    Class net("", "NetworkMessenger", Image("Assembly-CSharp.dll"));
    if (net.IsValid()) {
        G.netLocal = net.GetMethod("get_local", 0);
        for (auto &m : net.GetMethods(false))
            if (m.GetInfo() && m.GetInfo()->name && strcmp(m.GetInfo()->name, "RpcCreateItem") == 0) G.rpcCreateItem = m.GetInfo();
    }
    {
        Class pv("Photon.Pun", "PhotonView");
        G.tPhotonView = pv.IsValid() ? pv.GetMonoType() : nullptr;
        if (pv.IsValid()) G.pvRpc = Overload(pv, "RPC", {"System.String", "Photon.Pun.RpcTarget", "System.Object[]"});
        Class item("", "Item", Image("Assembly-CSharp.dll"));
        if (item.IsValid()) {
            G.tItem = item.GetMonoType();
            G.itemKey = item.GetMethod("get_itemKey", 0);
            G.itemHeld = item.GetMethod("IsHeldOrAnchoredToAnyPlayer", 0);
            G.itemId = item.GetMethod("get_itemID", 0);
            G.itemDrop = item.GetMethod("TrySendDrop", 0);
            G.setHeldItem = Class("", "MasterPlayer", Image("Assembly-CSharp.dll")).GetMethod("SetHeldItem", 2);
        }
        G.objectClass = Class("System", "Object").GetClass();
        G.stringClass = Class("System", "String").GetClass();
        G.arrayNew = (decltype(G.arrayNew))ym::api()->resolve("il2cpp_array_new");
        G.valueBox = (decltype(G.valueBox))ym::api()->resolve("il2cpp_value_box");
        ym::log("starter-menu: items rpc=%d pv=%d item=%d box=%d", G.rpcCreateItem != nullptr, G.pvRpc.IsValid(),
                G.itemKey.IsValid(), G.arrayNew && G.valueBox);
    }
    Class behaviour("UnityEngine", "Behaviour");
    G.behaviourGetEnabled = behaviour.GetMethod("get_enabled", 0);
    G.behaviourSetEnabled = behaviour.GetMethod("set_enabled", 1);
    // What moves your hands with the controllers (paused while you dance)
    for (auto nc : std::vector<std::pair<const char *, const char *>>{
             {"UnityEngine.XR.Interaction.Toolkit", "XRBaseController"},
             {"UnityEngine.InputSystem.XR", "TrackedPoseDriver"},
             {"UnityEngine.SpatialTracking", "TrackedPoseDriver"}}) {
        Class c(nc.first, nc.second);
        if (c.IsValid()) G.trackerTypes.push_back(c.GetMonoType());
    }
    G.tRigidbody = rb.GetMonoType();
    G.tCollider = Class("UnityEngine", "Collider").GetMonoType();
    G.tRenderer = rend.GetMonoType();
    G.tCanvas = canvas.GetMonoType();
    G.tText = text.GetMonoType();
    G.tFont = Class("UnityEngine", "Font").GetMonoType();
    G.usagePrimary.set((Il2CppObject *)CreateMonoString("PrimaryButton"));
    G.usageSecondary.set((Il2CppObject *)CreateMonoString("SecondaryButton"));
    G.usageGrip.set((Il2CppObject *)CreateMonoString("GripButton"));
    G.usageTrigger.set((Il2CppObject *)CreateMonoString("TriggerButton"));
    for (const char *s : {"Universal Render Pipeline/Unlit", "Universal Render Pipeline/Lit"})
        if (!G.shader.get() && G.shaderFind.IsValid()) G.shader.set(G.shaderFind((BNM::Structures::Mono::String *)CreateMonoString(s)));
    // Arial, like ii's menu (Unity 6 calls it LegacyRuntime.ttf)
    for (const char *f : {"LegacyRuntime.ttf", "Arial.ttf"})
        if (!G.font.get() && G.builtinResource.IsValid() && G.tFont)
            G.font.set(G.builtinResource((Il2CppObject *)G.tFont, CreateMonoString(f)));
    if (!G.font.get() && G.findAll.IsValid() && G.tFont) {   // else any font the game loaded (one time)
        auto *arr = (uint8_t *)G.findAll((Il2CppObject *)G.tFont);
        if (arr && *(uintptr_t *)(arr + 0x18) > 0) G.font.set(((Il2CppObject **)(arr + 0x20))[0]);
    }
    G.ok = G.createPrimitive.IsValid() && G.goTransform.IsValid() && G.trSetPosition.IsValid() &&
           G.deviceAtNode.IsValid() && G.tryGetBool.IsValid() && G.leftHand.IsValid();
    if (!G.ok) ym::log("starter-menu: some game methods are missing, menu off");
    return G.ok;
}

static bool Alive(Il2CppObject *o) { return o && G.alive((Il2CppObject *)o); }
static Il2CppObject *Tr(Il2CppObject *gameObject) { return G.goTransform[gameObject](); }
static void Show(Ref &r, bool on) { if (Alive(r.get())) G.goSetActive[r.get()](on); }
static void Kill(Ref &r) { if (Alive(r.get())) G.destroy(r.get()); r.clear(); }

// A primitive with a flat color. collider=false removes its collider so it can't push you.
// Always give new objects the URP shader: Unity's default material only draws in one
// eye on Quest.
static Il2CppObject *Prim(int type, Color c, bool collider) {
    Il2CppObject *o = G.createPrimitive(type);   // 0 = Sphere, 3 = Cube
    if (!o) return nullptr;
    if (!collider) if (auto *col = G.goGetComponent[o](G.tCollider)) G.destroy(col);
    if (auto *r = G.goGetComponent[o](G.tRenderer)) {
        Il2CppObject *m = G.rendererGetMaterial[r]();
        if (m && G.shader.get()) G.matSetShader[m](G.shader.get());
        if (m) G.matSetColor[m](c);
    }
    return o;
}
static Il2CppObject *Cube(Color c, bool collider) { return Prim(3, c, collider); }
static void SetColor(Il2CppObject *o, Color c) {
    if (!Alive(o)) return;
    if (auto *r = G.goGetComponent[o](G.tRenderer)) if (Il2CppObject *m = G.rendererGetMaterial[r]()) G.matSetColor[m](c);
}
static void SetText(Ref &text, const char *s) {
    if (Alive(text.get())) G.textSetText[text.get()](CreateMonoString(s));
}

// ---- input -------------------------------------------------------------------

struct Buttons { bool a = false, b = false, x = false, y = false, lgrip = false, rgrip = false, ltrig = false, rtrig = false; };
static bool Pressed(int node, Ref &usage) {
    XRDevice d = G.deviceAtNode(node);
    bool v = false;
    if (d.initialized) G.tryGetBool(d.id, (BNM::Structures::Mono::String *)usage.get(), &v);
    return v;
}
static Buttons ReadButtons() {
    const int left = 4, right = 5;              // XRNode.LeftHand / RightHand
    Buttons b;
    b.x = Pressed(left, G.usagePrimary);  b.y = Pressed(left, G.usageSecondary);
    b.a = Pressed(right, G.usagePrimary); b.b = Pressed(right, G.usageSecondary);
    b.lgrip = Pressed(left, G.usageGrip); b.rgrip = Pressed(right, G.usageGrip);
    b.ltrig = Pressed(left, G.usageTrigger); b.rtrig = Pressed(right, G.usageTrigger);
    return b;
}

// ---- menu contents -------------------------------------------------------------

// Feature switches. Every feature only changes your own game.
static bool f_velocityFly, f_ironMan, f_tpGun, f_noclip, f_speedBoost, f_noGravity;
static bool f_itemGun, f_spawnOnHand, f_car;
static char g_itemKey[64] = "";                // the last item you held (learned on grab)
struct PendingItem { Vector3 pos, vel; };
static std::vector<PendingItem> g_itemQueue;
static double g_nextItem = 0, g_nextBatch = 0;
static int g_dupeArmed = 0;

static constexpr int kDances = 128;        // 32 hand-made poses, then 96 generated ones (kGenFirst)
static constexpr int kGenFirst = 32;
static bool g_danceSel[kDances];
static int g_dance = -1;            // the picked dance, -1 = none
static bool f_fly, f_fastFly, f_platforms, f_lowGravity, f_rainbow, f_fps, f_clock, f_rightHand,
            f_notify = true;
static bool f_superJump, f_slowFall, f_airJump, f_dash, f_hover, f_bouncy, f_speedText, f_heightText;
static bool f_jetpack, f_autoTheme, f_clock24, f_haptics = true;
static Vector3 g_spot2;
static bool g_haveSpot2 = false;
static float g_menuSize = 1.0f;
static constexpr int kSoundCount = 23;
static char g_playLabel[64] = "Play 1/22: Twinkle Star";       // the Soundboard's Play button shows the picked sound
static char g_wavLabel[64] = "Play WAV: press Refresh Files";
static char g_textLabel[64] = "Text: (empty)";                      // the Item Text page shows what you typed here
static char g_textSpawnLabel[64] = "Spawn Next Line (0/0)";   // the My WAVs page shows the picked file here
static bool f_soundLoop = false;
static bool f_shareVoice = false, f_hearSelf = true;        // Everyone Hears It (off to start) and Hear It Yourself
static int g_soundIdx = 0;
static float g_soundVol = 0.6f;
static int g_borderStyle = 1, g_borderColor = 1;            // the border around the menu and its colors (1 = red and white)
static bool f_deco[12] = {true, true};                      // the decorations (Candy Canes and Pumpkins start on)
static int g_itemMult = 1;                                  // Item Multiplier: copies per Spawn On Hand press
static int g_flyLevel = 0, g_jumpLevel = 0;               // 0 = the normal speeds
static float g_dashPower = 12.0f, g_gravCancel = 0.7f, g_danceSpeed = 1.0f;
static Ref g_player, g_rigidbody, g_leftHand, g_rightHand, g_head;
static Vector3 g_spot;
static bool g_haveSpot = false;

static void Notify(const char *fmt, ...);
static void SaveSpot();
static void GoToSpot();
static void NextTheme();
static void AllOff();
static void PointerToHand();
static void NextPointer();
static void NextIronSpeed();
static void NextCarSpeed();
static void ApplySpeedBoost();
static void ApplyGravity();
static void DancePicked();
static void ArmDupe9();
static void ArmDupe27();
static void ArmCircle();
static void ArmSquare();
static void ArmTriangle();
static bool LeftHandItem();
static void NextMenuSize();
static void NextFunnySpeed();
static void SaveSpot2();
static void GoToSpot2();
static void TeleportForward();
static void TeleportUp();
static void ArmLine();
static void ArmSpiral();
static void ArmPyramid();
static void ArmHeart();
static void ArmCross();
static void ArmStar();
static void ArmDonut();
static void ArmDiamond();
static void ArmArrow();
static void ArmWave();
static void ArmTower();
static void ArmWall();
static void ArmBigGrid();
static void LaunchUp();
static void StopAction();
static void ClearItemQueue();
static void ItemMultOff();
static void PlaySelected();
static void NextSong();
static void PrevSong();
static void StopSound();
static void ApplyLoop();
static void ApplyShare();
static void VolUp();
static void VolDown();
static void RandomSound();
static void Snd0();
static void Snd1();
static void Snd2();
static void Snd3();
static void Snd4();
static void Snd5();
static void Snd6();
static void Snd7();
static void Snd8();
static void Snd9();
static void Snd10();
static void Snd11();
static void Snd12();
static void Snd13();
static void Snd14();
static void Snd15();
static void Snd16();
static void Snd17();
static void Snd18();
static void Snd19();
static void Snd20();
static void Snd21();
static void Snd22();
static void WavPlay();
static void WavNext();
static void WavPrev();
static void WavScan();
static void TextShow();
static void TextSpawnNext();
static void TextRestart();
static void TextBack();
static void TextSpace();
static void TextClear();
static void TextPreset();
static void KeyA();
static void KeyB();
static void KeyC();
static void KeyD();
static void KeyE();
static void KeyF();
static void KeyG();
static void KeyH();
static void KeyI();
static void KeyJ();
static void KeyK();
static void KeyL();
static void KeyM();
static void KeyN();
static void KeyO();
static void KeyP();
static void KeyQ();
static void KeyR();
static void KeyS();
static void KeyT();
static void KeyU();
static void KeyV();
static void KeyW();
static void KeyX();
static void KeyY();
static void KeyZ();
static void Key0();
static void Key1();
static void Key2();
static void Key3();
static void Key4();
static void Key5();
static void Key6();
static void Key7();
static void Key8();
static void Key9();
static void BorderStyle0();
static void BorderStyle1();
static void BorderStyle2();
static void BorderStyle3();
static void BorderStyle4();
static void BorderStyle5();
static void BorderStyle6();
static void BorderColor0();
static void BorderColor1();
static void BorderColor2();
static void BorderColor3();
static void BorderColor4();
static void BorderColor5();
static void BorderColor6();
static void BorderColor7();
static void BorderColor8();
static void BorderColor9();
static void BorderColor10();
static void BorderColor11();
static void ItemMult2();
static void ItemMult3();
static void ItemMult4();
static void ItemMult5();
static void ItemMult6();
static void ItemMult8();
static void ItemMult10();
static void ItemMult12();
static void ItemMult16();
static void ItemMult20();
static void PoseSpd0();
static void PoseSpd1();
static void PoseSpd2();
static void PoseSpd3();
static void PoseSpd4();
static void PoseSpd5();
static void PoseSpd6();
static void PoseSpd7();
static void PoseSpd8();
static void PoseSpd9();
static void ArmS0();
static void ArmS1();
static void ArmS2();
static void ArmS3();
static void ArmS4();
static void ArmS5();
static void ArmS6();
static void ArmS7();
static void ArmS8();
static void ArmS9();
static void ArmS10();
static void ArmS11();
static void ArmS12();
static void ArmS13();
static void ArmS14();
static void ArmS15();
static void ArmS16();
static void ArmS17();
static void ArmS18();
static void ArmS19();
static void ArmS20();
static void ArmS21();
static void ArmS22();
static void ArmS23();
static void ArmS24();
static void ArmS25();
static void ArmS26();
static void ArmS27();
static void ArmS28();
static void ArmS29();
static void ArmS30();
static void ArmS31();
static void ArmS32();
static void ArmS33();
static void ArmS34();
static void ArmS35();
static void ArmS36();
static void ArmS37();
static void ArmS38();
static void ArmS39();
static void ArmS40();
static void ArmS41();
static void ArmS42();
static void ArmS43();
static void ArmS44();
static void ArmS45();
static void ArmS46();
static void ArmS47();
static void ArmS48();
static void ArmS49();
static void ArmS50();
static void ArmS51();
static void ArmS52();
static void ArmS53();
static void ArmS54();
static void ArmS55();
static void ArmS56();
static void ArmS57();
static void ArmS58();
static void ArmS59();
static void ArmS60();
static void ArmS61();
static void ArmS62();
static void ArmS63();
static void ArmS64();
static void ArmS65();
static void ArmS66();
static void ArmS67();
static void ArmS68();
static void ArmS69();
static void ArmS70();
static void ArmS71();
static void ArmS72();
static void ArmS73();
static void ArmS74();
static void ArmS75();
static void ArmS76();
static void ArmS77();
static void ArmS78();
static void ArmS79();
static void ArmS80();
static void ArmS81();
static void ArmS82();
static void ArmS83();
static void ArmS84();
static void ArmS85();
static void ArmS86();
static void ArmS87();
static void ArmS88();
static void ArmS89();
static void ArmS90();
static void ArmS91();
static void FlyNormal();
static void JumpNormal();
static void FlyLvl1();
static void FlyLvl2();
static void FlyLvl3();
static void FlyLvl4();
static void FlyLvl5();
static void FlyLvl6();
static void FlyLvl7();
static void FlyLvl8();
static void FlyLvl9();
static void FlyLvl10();
static void FlyLvl11();
static void FlyLvl12();
static void FlyLvl13();
static void FlyLvl14();
static void FlyLvl15();
static void FlyLvl16();
static void FlyLvl17();
static void FlyLvl18();
static void FlyLvl19();
static void FlyLvl20();
static void JumpLvl1();
static void JumpLvl2();
static void JumpLvl3();
static void JumpLvl4();
static void JumpLvl5();
static void JumpLvl6();
static void JumpLvl7();
static void JumpLvl8();
static void JumpLvl9();
static void JumpLvl10();
static void JumpLvl11();
static void JumpLvl12();
static void JumpLvl13();
static void JumpLvl14();
static void JumpLvl15();
static void JumpLvl16();
static void JumpLvl17();
static void JumpLvl18();
static void JumpLvl19();
static void JumpLvl20();
static void DashLvl1();
static void DashLvl2();
static void DashLvl3();
static void DashLvl4();
static void DashLvl5();
static void DashLvl6();
static void DashLvl7();
static void DashLvl8();
static void DashLvl9();
static void DashLvl10();
static void GravNormal();
static void GravLvl0();
static void GravLvl10();
static void GravLvl20();
static void GravLvl30();
static void GravLvl40();
static void GravLvl50();
static void GravLvl60();
static void GravLvl70();
static void GravLvl80();
static void GravLvl90();
static void ThemeSet0();
static void ThemeSet1();
static void ThemeSet2();
static void ThemeSet3();
static void ThemeSet4();
static void ThemeSet5();
static void ThemeSet6();
static void ThemeSet7();
static void ThemeSet8();
static void ThemeSet9();
static void ThemeSet10();
static void ThemeSet11();
static void ThemeSet12();
static void ThemeSet13();
static void ThemeSet14();
static void ThemeSet15();
static void ThemeSet16();
static void ThemeSet17();
static void ThemeSet18();
static void ThemeSet19();
static void ThemeSet20();
static void ThemeSet21();
static void ThemeSet22();
static void ThemeSet23();
static void ThemeSet24();
static void ThemeSet25();
static void ThemeSet26();
static void ThemeSet27();
static void ThemeSet28();
static void ThemeSet29();
static void ThemeSet30();
static void ThemeSet31();
static void ThemeSet32();
static void ThemeSet33();
static void ThemeSet34();
static void ThemeSet35();
static void SizeSet0();
static void SizeSet1();
static void SizeSet2();
static void SizeSet3();
static void SizeSet4();
static void SizeSet5();
static void SizeSet6();
static void SizeSet7();
static void SizeSet8();
static void SizeSet9();
static bool GetVel(Vector3 &v);
static bool BodyPos(Vector3 &out);

enum Cat { MAIN, MOVEMENT, RIG, ITEMS, SETTINGS, ENABLED, POSES2, SHAPES, SPEED, GRAVITY, THEMES, SIZES, ITEMMULT, DECOR, SOUNDS, WAVS, ITEMTEXT, NCAT };
static const char *kCatName[NCAT] = {"Main", "Movement Mods", "Rig Mods", "Items", "Settings", "Enabled Mods",
                                     "Pose Pack", "Item Shapes", "Speed Levels", "Gravity Levels", "Themes", "Menu Sizes", "Item Multiplier", "Decorations", "Soundboard", "My WAVs", "Item Text"};
enum Kind { OPEN, TOGGLE, ACTION };
struct Item {
    Cat cat;
    const char *label;
    Kind kind;
    Cat target = MAIN;            // OPEN: the page it opens
    bool *flag = nullptr;         // TOGGLE: the switch
    void (*fn)() = nullptr;       // ACTION, or called after a TOGGLE changes
};
// The owner's layout (2026-10-31). Keep this order.
static Item g_items[] = {
    {MAIN, "Settings", OPEN, SETTINGS},
    {MAIN, "Movement Mods", OPEN, MOVEMENT},
    {MAIN, "Rig Mods", OPEN, RIG},
    {MAIN, "Items", OPEN, ITEMS},
    {MAIN, "Pose Pack", OPEN, POSES2},
    {MAIN, "Item Shapes", OPEN, SHAPES},
    {MAIN, "Speed Levels", OPEN, SPEED},
    {MAIN, "Gravity Levels", OPEN, GRAVITY},
    {MAIN, "Themes", OPEN, THEMES},
    {MAIN, "Menu Sizes", OPEN, SIZES},
    {MAIN, "Item Multiplier", OPEN, ITEMMULT},
    {MAIN, "Soundboard", OPEN, SOUNDS},
    {MAIN, "Decorations", OPEN, DECOR},
    {MAIN, "My WAVs", OPEN, WAVS},
    {MAIN, "Item Text", OPEN, ITEMTEXT},
    {MAIN, "Enabled Mods", OPEN, ENABLED},

    {MOVEMENT, "Exit Movement Mods", OPEN, MAIN},
    {MOVEMENT, "Platforms", TOGGLE, MAIN, &f_platforms},
    {MOVEMENT, "Fly", TOGGLE, MAIN, &f_fly},
    {MOVEMENT, "Fast Fly", TOGGLE, MAIN, &f_fastFly},
    {MOVEMENT, "Velocity Fly", TOGGLE, MAIN, &f_velocityFly},
    {MOVEMENT, "Car", TOGGLE, MAIN, &f_car},
    {MOVEMENT, "Teleport Gun", TOGGLE, MAIN, &f_tpGun},
    {MOVEMENT, "Iron Man", TOGGLE, MAIN, &f_ironMan},
    {MOVEMENT, "Noclip", TOGGLE, MAIN, &f_noclip},
    {MOVEMENT, "Speed Boost", TOGGLE, MAIN, &f_speedBoost, ApplySpeedBoost},
    {MOVEMENT, "No Gravity", TOGGLE, MAIN, &f_noGravity, ApplyGravity},
    {MOVEMENT, "Low Gravity", TOGGLE, MAIN, &f_lowGravity},
    {MOVEMENT, "Super Jump", TOGGLE, MAIN, &f_superJump, ApplySpeedBoost},
    {MOVEMENT, "Slow Fall", TOGGLE, MAIN, &f_slowFall},
    {MOVEMENT, "Air Jump", TOGGLE, MAIN, &f_airJump},
    {MOVEMENT, "Dash", TOGGLE, MAIN, &f_dash},
    {MOVEMENT, "Hover", TOGGLE, MAIN, &f_hover},
    {MOVEMENT, "Bouncy", TOGGLE, MAIN, &f_bouncy},
    {MOVEMENT, "Jetpack", TOGGLE, MAIN, &f_jetpack},
    {MOVEMENT, "Launch Up", ACTION, MAIN, nullptr, LaunchUp},
    {MOVEMENT, "Teleport Forward", ACTION, MAIN, nullptr, TeleportForward},
    {MOVEMENT, "Teleport Up", ACTION, MAIN, nullptr, TeleportUp},
    {MOVEMENT, "Save Spot 2", ACTION, MAIN, nullptr, SaveSpot2},
    {MOVEMENT, "Go To Spot 2", ACTION, MAIN, nullptr, GoToSpot2},
    {MOVEMENT, "Stop Moving", ACTION, MAIN, nullptr, StopAction},
    {MOVEMENT, "Save Spot", ACTION, MAIN, nullptr, SaveSpot},
    {MOVEMENT, "Go To Spot", ACTION, MAIN, nullptr, GoToSpot},


    {RIG, "Exit Rig Mods", OPEN, MAIN},
    {RIG, "Wave", TOGGLE, MAIN, &g_danceSel[0], DancePicked},
    {RIG, "Helicopter", TOGGLE, MAIN, &g_danceSel[1], DancePicked},
    {RIG, "T-Pose", TOGGLE, MAIN, &g_danceSel[2], DancePicked},
    {RIG, "Windmill", TOGGLE, MAIN, &g_danceSel[3], DancePicked},
    {RIG, "Swim", TOGGLE, MAIN, &g_danceSel[4], DancePicked},
    {RIG, "Jumping Jacks", TOGGLE, MAIN, &g_danceSel[5], DancePicked},
    {RIG, "Spaz", TOGGLE, MAIN, &g_danceSel[6], DancePicked},
    {RIG, "Beyblade", TOGGLE, MAIN, &g_danceSel[7], DancePicked},
    {RIG, "Superman", TOGGLE, MAIN, &g_danceSel[8], DancePicked},
    {RIG, "Flap", TOGGLE, MAIN, &g_danceSel[9], DancePicked},
    {RIG, "Boxing", TOGGLE, MAIN, &g_danceSel[10], DancePicked},
    {RIG, "Clap", TOGGLE, MAIN, &g_danceSel[11], DancePicked},
    {RIG, "Hands Up", TOGGLE, MAIN, &g_danceSel[12], DancePicked},
    {RIG, "Sprinkler", TOGGLE, MAIN, &g_danceSel[13], DancePicked},
    {RIG, "Funny Run", TOGGLE, MAIN, &g_danceSel[14], DancePicked},
    {RIG, "Funny Run Speed", ACTION, MAIN, nullptr, NextFunnySpeed},
    {RIG, "Spin", TOGGLE, MAIN, &g_danceSel[15], DancePicked},
    {RIG, "Upside Down", TOGGLE, MAIN, &g_danceSel[16], DancePicked},
    {RIG, "Floss", TOGGLE, MAIN, &g_danceSel[17], DancePicked},
    {RIG, "Dab", TOGGLE, MAIN, &g_danceSel[18], DancePicked},
    {RIG, "Salute", TOGGLE, MAIN, &g_danceSel[19], DancePicked},
    {RIG, "Zombie", TOGGLE, MAIN, &g_danceSel[20], DancePicked},
    {RIG, "Lasso", TOGGLE, MAIN, &g_danceSel[21], DancePicked},
    {RIG, "Chicken", TOGGLE, MAIN, &g_danceSel[22], DancePicked},
    {RIG, "Robot", TOGGLE, MAIN, &g_danceSel[23], DancePicked},
    {RIG, "Heart", TOGGLE, MAIN, &g_danceSel[24], DancePicked},
    {RIG, "Flex", TOGGLE, MAIN, &g_danceSel[25], DancePicked},
    {RIG, "Shrug", TOGGLE, MAIN, &g_danceSel[26], DancePicked},
    {RIG, "Conductor", TOGGLE, MAIN, &g_danceSel[27], DancePicked},
    {RIG, "Both Waving", TOGGLE, MAIN, &g_danceSel[28], DancePicked},
    {RIG, "Dead", TOGGLE, MAIN, &g_danceSel[29], DancePicked},
    {RIG, "Corpse", TOGGLE, MAIN, &g_danceSel[30], DancePicked},
    {RIG, "Collapse", TOGGLE, MAIN, &g_danceSel[31], DancePicked},

    {ITEMS, "Exit Items", OPEN, MAIN},
    {ITEMS, "Clear Item Queue", ACTION, MAIN, nullptr, ClearItemQueue},
    {ITEMS, "Spawn Item Gun", TOGGLE, MAIN, &f_itemGun},
    {ITEMS, "Duplicate Items (3x3)", ACTION, MAIN, nullptr, ArmDupe9},
    {ITEMS, "Duplicate Items (3x3x3)", ACTION, MAIN, nullptr, ArmDupe27},
    {ITEMS, "Duplicate Items (Circle)", ACTION, MAIN, nullptr, ArmCircle},
    {ITEMS, "Big Square (4x4)", ACTION, MAIN, nullptr, ArmSquare},
    {ITEMS, "Big Triangle", ACTION, MAIN, nullptr, ArmTriangle},
    {ITEMS, "Line (10)", ACTION, MAIN, nullptr, ArmLine},
    {ITEMS, "Spiral", ACTION, MAIN, nullptr, ArmSpiral},
    {ITEMS, "Pyramid", ACTION, MAIN, nullptr, ArmPyramid},
    {ITEMS, "Heart", ACTION, MAIN, nullptr, ArmHeart},
    {ITEMS, "Cross", ACTION, MAIN, nullptr, ArmCross},
    {ITEMS, "Star", ACTION, MAIN, nullptr, ArmStar},
    {ITEMS, "Donut", ACTION, MAIN, nullptr, ArmDonut},
    {ITEMS, "Diamond", ACTION, MAIN, nullptr, ArmDiamond},
    {ITEMS, "Arrow", ACTION, MAIN, nullptr, ArmArrow},
    {ITEMS, "Wave", ACTION, MAIN, nullptr, ArmWave},
    {ITEMS, "Tower (8)", ACTION, MAIN, nullptr, ArmTower},
    {ITEMS, "Wall (5x3)", ACTION, MAIN, nullptr, ArmWall},
    {ITEMS, "Big Grid (5x5)", ACTION, MAIN, nullptr, ArmBigGrid},
    {ITEMS, "Spawn On Hand", TOGGLE, MAIN, &f_spawnOnHand},

    {POSES2, "Exit Pose Pack", OPEN, MAIN},
    {POSES2, "Pose Speed 0.5x", ACTION, MAIN, nullptr, PoseSpd0},
    {POSES2, "Pose Speed 0.6x", ACTION, MAIN, nullptr, PoseSpd1},
    {POSES2, "Pose Speed 0.7x", ACTION, MAIN, nullptr, PoseSpd2},
    {POSES2, "Pose Speed 0.8x", ACTION, MAIN, nullptr, PoseSpd3},
    {POSES2, "Pose Speed 0.9x", ACTION, MAIN, nullptr, PoseSpd4},
    {POSES2, "Pose Speed 1x", ACTION, MAIN, nullptr, PoseSpd5},
    {POSES2, "Pose Speed 1.2x", ACTION, MAIN, nullptr, PoseSpd6},
    {POSES2, "Pose Speed 1.5x", ACTION, MAIN, nullptr, PoseSpd7},
    {POSES2, "Pose Speed 1.8x", ACTION, MAIN, nullptr, PoseSpd8},
    {POSES2, "Pose Speed 2x", ACTION, MAIN, nullptr, PoseSpd9},
    {POSES2, "Circles Slow S Sync", TOGGLE, MAIN, &g_danceSel[32], DancePicked},
    {POSES2, "Circles Slow S Alt", TOGGLE, MAIN, &g_danceSel[33], DancePicked},
    {POSES2, "Circles Slow L Sync", TOGGLE, MAIN, &g_danceSel[34], DancePicked},
    {POSES2, "Circles Slow L Alt", TOGGLE, MAIN, &g_danceSel[35], DancePicked},
    {POSES2, "Circles Mid S Sync", TOGGLE, MAIN, &g_danceSel[36], DancePicked},
    {POSES2, "Circles Mid S Alt", TOGGLE, MAIN, &g_danceSel[37], DancePicked},
    {POSES2, "Circles Mid L Sync", TOGGLE, MAIN, &g_danceSel[38], DancePicked},
    {POSES2, "Circles Mid L Alt", TOGGLE, MAIN, &g_danceSel[39], DancePicked},
    {POSES2, "Circles Fast S Sync", TOGGLE, MAIN, &g_danceSel[40], DancePicked},
    {POSES2, "Circles Fast S Alt", TOGGLE, MAIN, &g_danceSel[41], DancePicked},
    {POSES2, "Circles Fast L Sync", TOGGLE, MAIN, &g_danceSel[42], DancePicked},
    {POSES2, "Circles Fast L Alt", TOGGLE, MAIN, &g_danceSel[43], DancePicked},
    {POSES2, "Figure 8 Slow S Sync", TOGGLE, MAIN, &g_danceSel[44], DancePicked},
    {POSES2, "Figure 8 Slow S Alt", TOGGLE, MAIN, &g_danceSel[45], DancePicked},
    {POSES2, "Figure 8 Slow L Sync", TOGGLE, MAIN, &g_danceSel[46], DancePicked},
    {POSES2, "Figure 8 Slow L Alt", TOGGLE, MAIN, &g_danceSel[47], DancePicked},
    {POSES2, "Figure 8 Mid S Sync", TOGGLE, MAIN, &g_danceSel[48], DancePicked},
    {POSES2, "Figure 8 Mid S Alt", TOGGLE, MAIN, &g_danceSel[49], DancePicked},
    {POSES2, "Figure 8 Mid L Sync", TOGGLE, MAIN, &g_danceSel[50], DancePicked},
    {POSES2, "Figure 8 Mid L Alt", TOGGLE, MAIN, &g_danceSel[51], DancePicked},
    {POSES2, "Figure 8 Fast S Sync", TOGGLE, MAIN, &g_danceSel[52], DancePicked},
    {POSES2, "Figure 8 Fast S Alt", TOGGLE, MAIN, &g_danceSel[53], DancePicked},
    {POSES2, "Figure 8 Fast L Sync", TOGGLE, MAIN, &g_danceSel[54], DancePicked},
    {POSES2, "Figure 8 Fast L Alt", TOGGLE, MAIN, &g_danceSel[55], DancePicked},
    {POSES2, "Sweeps Slow S Sync", TOGGLE, MAIN, &g_danceSel[56], DancePicked},
    {POSES2, "Sweeps Slow S Alt", TOGGLE, MAIN, &g_danceSel[57], DancePicked},
    {POSES2, "Sweeps Slow L Sync", TOGGLE, MAIN, &g_danceSel[58], DancePicked},
    {POSES2, "Sweeps Slow L Alt", TOGGLE, MAIN, &g_danceSel[59], DancePicked},
    {POSES2, "Sweeps Mid S Sync", TOGGLE, MAIN, &g_danceSel[60], DancePicked},
    {POSES2, "Sweeps Mid S Alt", TOGGLE, MAIN, &g_danceSel[61], DancePicked},
    {POSES2, "Sweeps Mid L Sync", TOGGLE, MAIN, &g_danceSel[62], DancePicked},
    {POSES2, "Sweeps Mid L Alt", TOGGLE, MAIN, &g_danceSel[63], DancePicked},
    {POSES2, "Sweeps Fast S Sync", TOGGLE, MAIN, &g_danceSel[64], DancePicked},
    {POSES2, "Sweeps Fast S Alt", TOGGLE, MAIN, &g_danceSel[65], DancePicked},
    {POSES2, "Sweeps Fast L Sync", TOGGLE, MAIN, &g_danceSel[66], DancePicked},
    {POSES2, "Sweeps Fast L Alt", TOGGLE, MAIN, &g_danceSel[67], DancePicked},
    {POSES2, "Pumps Slow S Sync", TOGGLE, MAIN, &g_danceSel[68], DancePicked},
    {POSES2, "Pumps Slow S Alt", TOGGLE, MAIN, &g_danceSel[69], DancePicked},
    {POSES2, "Pumps Slow L Sync", TOGGLE, MAIN, &g_danceSel[70], DancePicked},
    {POSES2, "Pumps Slow L Alt", TOGGLE, MAIN, &g_danceSel[71], DancePicked},
    {POSES2, "Pumps Mid S Sync", TOGGLE, MAIN, &g_danceSel[72], DancePicked},
    {POSES2, "Pumps Mid S Alt", TOGGLE, MAIN, &g_danceSel[73], DancePicked},
    {POSES2, "Pumps Mid L Sync", TOGGLE, MAIN, &g_danceSel[74], DancePicked},
    {POSES2, "Pumps Mid L Alt", TOGGLE, MAIN, &g_danceSel[75], DancePicked},
    {POSES2, "Pumps Fast S Sync", TOGGLE, MAIN, &g_danceSel[76], DancePicked},
    {POSES2, "Pumps Fast S Alt", TOGGLE, MAIN, &g_danceSel[77], DancePicked},
    {POSES2, "Pumps Fast L Sync", TOGGLE, MAIN, &g_danceSel[78], DancePicked},
    {POSES2, "Pumps Fast L Alt", TOGGLE, MAIN, &g_danceSel[79], DancePicked},
    {POSES2, "Bobs Slow S Sync", TOGGLE, MAIN, &g_danceSel[80], DancePicked},
    {POSES2, "Bobs Slow S Alt", TOGGLE, MAIN, &g_danceSel[81], DancePicked},
    {POSES2, "Bobs Slow L Sync", TOGGLE, MAIN, &g_danceSel[82], DancePicked},
    {POSES2, "Bobs Slow L Alt", TOGGLE, MAIN, &g_danceSel[83], DancePicked},
    {POSES2, "Bobs Mid S Sync", TOGGLE, MAIN, &g_danceSel[84], DancePicked},
    {POSES2, "Bobs Mid S Alt", TOGGLE, MAIN, &g_danceSel[85], DancePicked},
    {POSES2, "Bobs Mid L Sync", TOGGLE, MAIN, &g_danceSel[86], DancePicked},
    {POSES2, "Bobs Mid L Alt", TOGGLE, MAIN, &g_danceSel[87], DancePicked},
    {POSES2, "Bobs Fast S Sync", TOGGLE, MAIN, &g_danceSel[88], DancePicked},
    {POSES2, "Bobs Fast S Alt", TOGGLE, MAIN, &g_danceSel[89], DancePicked},
    {POSES2, "Bobs Fast L Sync", TOGGLE, MAIN, &g_danceSel[90], DancePicked},
    {POSES2, "Bobs Fast L Alt", TOGGLE, MAIN, &g_danceSel[91], DancePicked},
    {POSES2, "Flaps Slow S Sync", TOGGLE, MAIN, &g_danceSel[92], DancePicked},
    {POSES2, "Flaps Slow S Alt", TOGGLE, MAIN, &g_danceSel[93], DancePicked},
    {POSES2, "Flaps Slow L Sync", TOGGLE, MAIN, &g_danceSel[94], DancePicked},
    {POSES2, "Flaps Slow L Alt", TOGGLE, MAIN, &g_danceSel[95], DancePicked},
    {POSES2, "Flaps Mid S Sync", TOGGLE, MAIN, &g_danceSel[96], DancePicked},
    {POSES2, "Flaps Mid S Alt", TOGGLE, MAIN, &g_danceSel[97], DancePicked},
    {POSES2, "Flaps Mid L Sync", TOGGLE, MAIN, &g_danceSel[98], DancePicked},
    {POSES2, "Flaps Mid L Alt", TOGGLE, MAIN, &g_danceSel[99], DancePicked},
    {POSES2, "Flaps Fast S Sync", TOGGLE, MAIN, &g_danceSel[100], DancePicked},
    {POSES2, "Flaps Fast S Alt", TOGGLE, MAIN, &g_danceSel[101], DancePicked},
    {POSES2, "Flaps Fast L Sync", TOGGLE, MAIN, &g_danceSel[102], DancePicked},
    {POSES2, "Flaps Fast L Alt", TOGGLE, MAIN, &g_danceSel[103], DancePicked},
    {POSES2, "Spins Slow S Sync", TOGGLE, MAIN, &g_danceSel[104], DancePicked},
    {POSES2, "Spins Slow S Alt", TOGGLE, MAIN, &g_danceSel[105], DancePicked},
    {POSES2, "Spins Slow L Sync", TOGGLE, MAIN, &g_danceSel[106], DancePicked},
    {POSES2, "Spins Slow L Alt", TOGGLE, MAIN, &g_danceSel[107], DancePicked},
    {POSES2, "Spins Mid S Sync", TOGGLE, MAIN, &g_danceSel[108], DancePicked},
    {POSES2, "Spins Mid S Alt", TOGGLE, MAIN, &g_danceSel[109], DancePicked},
    {POSES2, "Spins Mid L Sync", TOGGLE, MAIN, &g_danceSel[110], DancePicked},
    {POSES2, "Spins Mid L Alt", TOGGLE, MAIN, &g_danceSel[111], DancePicked},
    {POSES2, "Spins Fast S Sync", TOGGLE, MAIN, &g_danceSel[112], DancePicked},
    {POSES2, "Spins Fast S Alt", TOGGLE, MAIN, &g_danceSel[113], DancePicked},
    {POSES2, "Spins Fast L Sync", TOGGLE, MAIN, &g_danceSel[114], DancePicked},
    {POSES2, "Spins Fast L Alt", TOGGLE, MAIN, &g_danceSel[115], DancePicked},
    {POSES2, "Waves Slow S Sync", TOGGLE, MAIN, &g_danceSel[116], DancePicked},
    {POSES2, "Waves Slow S Alt", TOGGLE, MAIN, &g_danceSel[117], DancePicked},
    {POSES2, "Waves Slow L Sync", TOGGLE, MAIN, &g_danceSel[118], DancePicked},
    {POSES2, "Waves Slow L Alt", TOGGLE, MAIN, &g_danceSel[119], DancePicked},
    {POSES2, "Waves Mid S Sync", TOGGLE, MAIN, &g_danceSel[120], DancePicked},
    {POSES2, "Waves Mid S Alt", TOGGLE, MAIN, &g_danceSel[121], DancePicked},
    {POSES2, "Waves Mid L Sync", TOGGLE, MAIN, &g_danceSel[122], DancePicked},
    {POSES2, "Waves Mid L Alt", TOGGLE, MAIN, &g_danceSel[123], DancePicked},
    {POSES2, "Waves Fast S Sync", TOGGLE, MAIN, &g_danceSel[124], DancePicked},
    {POSES2, "Waves Fast S Alt", TOGGLE, MAIN, &g_danceSel[125], DancePicked},
    {POSES2, "Waves Fast L Sync", TOGGLE, MAIN, &g_danceSel[126], DancePicked},
    {POSES2, "Waves Fast L Alt", TOGGLE, MAIN, &g_danceSel[127], DancePicked},
    {SHAPES, "Exit Item Shapes", OPEN, MAIN},
    {SHAPES, "Ring 3", ACTION, MAIN, nullptr, ArmS0},
    {SHAPES, "Ring 4", ACTION, MAIN, nullptr, ArmS1},
    {SHAPES, "Ring 5", ACTION, MAIN, nullptr, ArmS2},
    {SHAPES, "Ring 6", ACTION, MAIN, nullptr, ArmS3},
    {SHAPES, "Ring 7", ACTION, MAIN, nullptr, ArmS4},
    {SHAPES, "Ring 8", ACTION, MAIN, nullptr, ArmS5},
    {SHAPES, "Ring 9", ACTION, MAIN, nullptr, ArmS6},
    {SHAPES, "Ring 10", ACTION, MAIN, nullptr, ArmS7},
    {SHAPES, "Ring 11", ACTION, MAIN, nullptr, ArmS8},
    {SHAPES, "Ring 12", ACTION, MAIN, nullptr, ArmS9},
    {SHAPES, "Ring 13", ACTION, MAIN, nullptr, ArmS10},
    {SHAPES, "Ring 14", ACTION, MAIN, nullptr, ArmS11},
    {SHAPES, "Ring 15", ACTION, MAIN, nullptr, ArmS12},
    {SHAPES, "Ring 16", ACTION, MAIN, nullptr, ArmS13},
    {SHAPES, "Star 4 Point", ACTION, MAIN, nullptr, ArmS14},
    {SHAPES, "Star 5 Point", ACTION, MAIN, nullptr, ArmS15},
    {SHAPES, "Star 6 Point", ACTION, MAIN, nullptr, ArmS16},
    {SHAPES, "Star 7 Point", ACTION, MAIN, nullptr, ArmS17},
    {SHAPES, "Star 8 Point", ACTION, MAIN, nullptr, ArmS18},
    {SHAPES, "Star 9 Point", ACTION, MAIN, nullptr, ArmS19},
    {SHAPES, "Star 10 Point", ACTION, MAIN, nullptr, ArmS20},
    {SHAPES, "Star 11 Point", ACTION, MAIN, nullptr, ArmS21},
    {SHAPES, "Star 12 Point", ACTION, MAIN, nullptr, ArmS22},
    {SHAPES, "Row 3", ACTION, MAIN, nullptr, ArmS23},
    {SHAPES, "Row 4", ACTION, MAIN, nullptr, ArmS24},
    {SHAPES, "Row 5", ACTION, MAIN, nullptr, ArmS25},
    {SHAPES, "Row 6", ACTION, MAIN, nullptr, ArmS26},
    {SHAPES, "Row 7", ACTION, MAIN, nullptr, ArmS27},
    {SHAPES, "Row 8", ACTION, MAIN, nullptr, ArmS28},
    {SHAPES, "Row 9", ACTION, MAIN, nullptr, ArmS29},
    {SHAPES, "Row 10", ACTION, MAIN, nullptr, ArmS30},
    {SHAPES, "Row 11", ACTION, MAIN, nullptr, ArmS31},
    {SHAPES, "Row 12", ACTION, MAIN, nullptr, ArmS32},
    {SHAPES, "Grid 2x3", ACTION, MAIN, nullptr, ArmS33},
    {SHAPES, "Grid 2x4", ACTION, MAIN, nullptr, ArmS34},
    {SHAPES, "Grid 2x5", ACTION, MAIN, nullptr, ArmS35},
    {SHAPES, "Grid 2x6", ACTION, MAIN, nullptr, ArmS36},
    {SHAPES, "Grid 3x4", ACTION, MAIN, nullptr, ArmS37},
    {SHAPES, "Grid 3x5", ACTION, MAIN, nullptr, ArmS38},
    {SHAPES, "Grid 3x6", ACTION, MAIN, nullptr, ArmS39},
    {SHAPES, "Grid 4x5", ACTION, MAIN, nullptr, ArmS40},
    {SHAPES, "Grid 4x6", ACTION, MAIN, nullptr, ArmS41},
    {SHAPES, "Spiral S", ACTION, MAIN, nullptr, ArmS42},
    {SHAPES, "Spiral M", ACTION, MAIN, nullptr, ArmS43},
    {SHAPES, "Spiral L", ACTION, MAIN, nullptr, ArmS44},
    {SHAPES, "Box 2x2x2", ACTION, MAIN, nullptr, ArmS45},
    {SHAPES, "Box 3x2x3", ACTION, MAIN, nullptr, ArmS46},
    {SHAPES, "Box 2x3x2", ACTION, MAIN, nullptr, ArmS47},
    {SHAPES, "Box 3x3x2", ACTION, MAIN, nullptr, ArmS48},
    {SHAPES, "Box 2x2x4", ACTION, MAIN, nullptr, ArmS49},
    {SHAPES, "Box 4x2x3", ACTION, MAIN, nullptr, ArmS50},
    {SHAPES, "Box 3x2x4", ACTION, MAIN, nullptr, ArmS51},
    {SHAPES, "Box 2x4x3", ACTION, MAIN, nullptr, ArmS52},
    {SHAPES, "Wall 3x2", ACTION, MAIN, nullptr, ArmS53},
    {SHAPES, "Wall 4x2", ACTION, MAIN, nullptr, ArmS54},
    {SHAPES, "Wall 5x2", ACTION, MAIN, nullptr, ArmS55},
    {SHAPES, "Wall 6x2", ACTION, MAIN, nullptr, ArmS56},
    {SHAPES, "Wall 3x3", ACTION, MAIN, nullptr, ArmS57},
    {SHAPES, "Wall 4x3", ACTION, MAIN, nullptr, ArmS58},
    {SHAPES, "Wall 6x3", ACTION, MAIN, nullptr, ArmS59},
    {SHAPES, "Wall 4x4", ACTION, MAIN, nullptr, ArmS60},
    {SHAPES, "Wall 5x4", ACTION, MAIN, nullptr, ArmS61},
    {SHAPES, "Wall 6x4", ACTION, MAIN, nullptr, ArmS62},
    {SHAPES, "Tower 2", ACTION, MAIN, nullptr, ArmS63},
    {SHAPES, "Tower 3", ACTION, MAIN, nullptr, ArmS64},
    {SHAPES, "Tower 4", ACTION, MAIN, nullptr, ArmS65},
    {SHAPES, "Tower 5", ACTION, MAIN, nullptr, ArmS66},
    {SHAPES, "Tower 6", ACTION, MAIN, nullptr, ArmS67},
    {SHAPES, "Tower 7", ACTION, MAIN, nullptr, ArmS68},
    {SHAPES, "Tower 8", ACTION, MAIN, nullptr, ArmS69},
    {SHAPES, "Tower 9", ACTION, MAIN, nullptr, ArmS70},
    {SHAPES, "Tower 10", ACTION, MAIN, nullptr, ArmS71},
    {SHAPES, "Tower 11", ACTION, MAIN, nullptr, ArmS72},
    {SHAPES, "Tower 12", ACTION, MAIN, nullptr, ArmS73},
    {SHAPES, "Zigzag 6", ACTION, MAIN, nullptr, ArmS74},
    {SHAPES, "Zigzag 8", ACTION, MAIN, nullptr, ArmS75},
    {SHAPES, "Zigzag 10", ACTION, MAIN, nullptr, ArmS76},
    {SHAPES, "Zigzag 12", ACTION, MAIN, nullptr, ArmS77},
    {SHAPES, "Zigzag 16", ACTION, MAIN, nullptr, ArmS78},
    {SHAPES, "Zigzag 20", ACTION, MAIN, nullptr, ArmS79},
    {SHAPES, "Plus 2", ACTION, MAIN, nullptr, ArmS80},
    {SHAPES, "Plus 3", ACTION, MAIN, nullptr, ArmS81},
    {SHAPES, "Plus 4", ACTION, MAIN, nullptr, ArmS82},
    {SHAPES, "Plus 5", ACTION, MAIN, nullptr, ArmS83},
    {SHAPES, "Plus 6", ACTION, MAIN, nullptr, ArmS84},
    {SHAPES, "Heart 12", ACTION, MAIN, nullptr, ArmS85},
    {SHAPES, "Heart 20", ACTION, MAIN, nullptr, ArmS86},
    {SHAPES, "Heart 24", ACTION, MAIN, nullptr, ArmS87},
    {SHAPES, "Helix 10", ACTION, MAIN, nullptr, ArmS88},
    {SHAPES, "Helix 14", ACTION, MAIN, nullptr, ArmS89},
    {SHAPES, "Helix 18", ACTION, MAIN, nullptr, ArmS90},
    {SHAPES, "Helix 22", ACTION, MAIN, nullptr, ArmS91},
    {SPEED, "Exit Speed Levels", OPEN, MAIN},
    {SPEED, "Fly Speed Normal", ACTION, MAIN, nullptr, FlyNormal},
    {SPEED, "Jump Normal", ACTION, MAIN, nullptr, JumpNormal},
    {SPEED, "Fly Speed 1", ACTION, MAIN, nullptr, FlyLvl1},
    {SPEED, "Fly Speed 2", ACTION, MAIN, nullptr, FlyLvl2},
    {SPEED, "Fly Speed 3", ACTION, MAIN, nullptr, FlyLvl3},
    {SPEED, "Fly Speed 4", ACTION, MAIN, nullptr, FlyLvl4},
    {SPEED, "Fly Speed 5", ACTION, MAIN, nullptr, FlyLvl5},
    {SPEED, "Fly Speed 6", ACTION, MAIN, nullptr, FlyLvl6},
    {SPEED, "Fly Speed 7", ACTION, MAIN, nullptr, FlyLvl7},
    {SPEED, "Fly Speed 8", ACTION, MAIN, nullptr, FlyLvl8},
    {SPEED, "Fly Speed 9", ACTION, MAIN, nullptr, FlyLvl9},
    {SPEED, "Fly Speed 10", ACTION, MAIN, nullptr, FlyLvl10},
    {SPEED, "Fly Speed 11", ACTION, MAIN, nullptr, FlyLvl11},
    {SPEED, "Fly Speed 12", ACTION, MAIN, nullptr, FlyLvl12},
    {SPEED, "Fly Speed 13", ACTION, MAIN, nullptr, FlyLvl13},
    {SPEED, "Fly Speed 14", ACTION, MAIN, nullptr, FlyLvl14},
    {SPEED, "Fly Speed 15", ACTION, MAIN, nullptr, FlyLvl15},
    {SPEED, "Fly Speed 16", ACTION, MAIN, nullptr, FlyLvl16},
    {SPEED, "Fly Speed 17", ACTION, MAIN, nullptr, FlyLvl17},
    {SPEED, "Fly Speed 18", ACTION, MAIN, nullptr, FlyLvl18},
    {SPEED, "Fly Speed 19", ACTION, MAIN, nullptr, FlyLvl19},
    {SPEED, "Fly Speed 20", ACTION, MAIN, nullptr, FlyLvl20},
    {SPEED, "Jump Level 1", ACTION, MAIN, nullptr, JumpLvl1},
    {SPEED, "Jump Level 2", ACTION, MAIN, nullptr, JumpLvl2},
    {SPEED, "Jump Level 3", ACTION, MAIN, nullptr, JumpLvl3},
    {SPEED, "Jump Level 4", ACTION, MAIN, nullptr, JumpLvl4},
    {SPEED, "Jump Level 5", ACTION, MAIN, nullptr, JumpLvl5},
    {SPEED, "Jump Level 6", ACTION, MAIN, nullptr, JumpLvl6},
    {SPEED, "Jump Level 7", ACTION, MAIN, nullptr, JumpLvl7},
    {SPEED, "Jump Level 8", ACTION, MAIN, nullptr, JumpLvl8},
    {SPEED, "Jump Level 9", ACTION, MAIN, nullptr, JumpLvl9},
    {SPEED, "Jump Level 10", ACTION, MAIN, nullptr, JumpLvl10},
    {SPEED, "Jump Level 11", ACTION, MAIN, nullptr, JumpLvl11},
    {SPEED, "Jump Level 12", ACTION, MAIN, nullptr, JumpLvl12},
    {SPEED, "Jump Level 13", ACTION, MAIN, nullptr, JumpLvl13},
    {SPEED, "Jump Level 14", ACTION, MAIN, nullptr, JumpLvl14},
    {SPEED, "Jump Level 15", ACTION, MAIN, nullptr, JumpLvl15},
    {SPEED, "Jump Level 16", ACTION, MAIN, nullptr, JumpLvl16},
    {SPEED, "Jump Level 17", ACTION, MAIN, nullptr, JumpLvl17},
    {SPEED, "Jump Level 18", ACTION, MAIN, nullptr, JumpLvl18},
    {SPEED, "Jump Level 19", ACTION, MAIN, nullptr, JumpLvl19},
    {SPEED, "Jump Level 20", ACTION, MAIN, nullptr, JumpLvl20},
    {SPEED, "Dash Power 1", ACTION, MAIN, nullptr, DashLvl1},
    {SPEED, "Dash Power 2", ACTION, MAIN, nullptr, DashLvl2},
    {SPEED, "Dash Power 3", ACTION, MAIN, nullptr, DashLvl3},
    {SPEED, "Dash Power 4", ACTION, MAIN, nullptr, DashLvl4},
    {SPEED, "Dash Power 5", ACTION, MAIN, nullptr, DashLvl5},
    {SPEED, "Dash Power 6", ACTION, MAIN, nullptr, DashLvl6},
    {SPEED, "Dash Power 7", ACTION, MAIN, nullptr, DashLvl7},
    {SPEED, "Dash Power 8", ACTION, MAIN, nullptr, DashLvl8},
    {SPEED, "Dash Power 9", ACTION, MAIN, nullptr, DashLvl9},
    {SPEED, "Dash Power 10", ACTION, MAIN, nullptr, DashLvl10},
    {GRAVITY, "Exit Gravity Levels", OPEN, MAIN},
    {GRAVITY, "Gravity Normal", ACTION, MAIN, nullptr, GravNormal},
    {GRAVITY, "Gravity 0%", ACTION, MAIN, nullptr, GravLvl0},
    {GRAVITY, "Gravity 10%", ACTION, MAIN, nullptr, GravLvl10},
    {GRAVITY, "Gravity 20%", ACTION, MAIN, nullptr, GravLvl20},
    {GRAVITY, "Gravity 30%", ACTION, MAIN, nullptr, GravLvl30},
    {GRAVITY, "Gravity 40%", ACTION, MAIN, nullptr, GravLvl40},
    {GRAVITY, "Gravity 50%", ACTION, MAIN, nullptr, GravLvl50},
    {GRAVITY, "Gravity 60%", ACTION, MAIN, nullptr, GravLvl60},
    {GRAVITY, "Gravity 70%", ACTION, MAIN, nullptr, GravLvl70},
    {GRAVITY, "Gravity 80%", ACTION, MAIN, nullptr, GravLvl80},
    {GRAVITY, "Gravity 90%", ACTION, MAIN, nullptr, GravLvl90},
    {THEMES, "Exit Themes", OPEN, MAIN},
    {THEMES, "Ruby", ACTION, MAIN, nullptr, ThemeSet0},
    {THEMES, "Scarlet", ACTION, MAIN, nullptr, ThemeSet1},
    {THEMES, "Coral", ACTION, MAIN, nullptr, ThemeSet2},
    {THEMES, "Ember", ACTION, MAIN, nullptr, ThemeSet3},
    {THEMES, "Amber", ACTION, MAIN, nullptr, ThemeSet4},
    {THEMES, "Honey", ACTION, MAIN, nullptr, ThemeSet5},
    {THEMES, "Lemon", ACTION, MAIN, nullptr, ThemeSet6},
    {THEMES, "Lime", ACTION, MAIN, nullptr, ThemeSet7},
    {THEMES, "Fern", ACTION, MAIN, nullptr, ThemeSet8},
    {THEMES, "Jade", ACTION, MAIN, nullptr, ThemeSet9},
    {THEMES, "Emerald", ACTION, MAIN, nullptr, ThemeSet10},
    {THEMES, "Spring", ACTION, MAIN, nullptr, ThemeSet11},
    {THEMES, "Teal", ACTION, MAIN, nullptr, ThemeSet12},
    {THEMES, "Aqua", ACTION, MAIN, nullptr, ThemeSet13},
    {THEMES, "Azure", ACTION, MAIN, nullptr, ThemeSet14},
    {THEMES, "Sky", ACTION, MAIN, nullptr, ThemeSet15},
    {THEMES, "Cobalt", ACTION, MAIN, nullptr, ThemeSet16},
    {THEMES, "Sapphire", ACTION, MAIN, nullptr, ThemeSet17},
    {THEMES, "Indigo", ACTION, MAIN, nullptr, ThemeSet18},
    {THEMES, "Violet", ACTION, MAIN, nullptr, ThemeSet19},
    {THEMES, "Grape", ACTION, MAIN, nullptr, ThemeSet20},
    {THEMES, "Orchid", ACTION, MAIN, nullptr, ThemeSet21},
    {THEMES, "Magenta", ACTION, MAIN, nullptr, ThemeSet22},
    {THEMES, "Fuchsia", ACTION, MAIN, nullptr, ThemeSet23},
    {THEMES, "Rose", ACTION, MAIN, nullptr, ThemeSet24},
    {THEMES, "Blush", ACTION, MAIN, nullptr, ThemeSet25},
    {THEMES, "Cherry", ACTION, MAIN, nullptr, ThemeSet26},
    {THEMES, "Cranberry", ACTION, MAIN, nullptr, ThemeSet27},
    {THEMES, "Peach", ACTION, MAIN, nullptr, ThemeSet28},
    {THEMES, "Mango", ACTION, MAIN, nullptr, ThemeSet29},
    {THEMES, "Moss", ACTION, MAIN, nullptr, ThemeSet30},
    {THEMES, "Pine", ACTION, MAIN, nullptr, ThemeSet31},
    {THEMES, "Ocean", ACTION, MAIN, nullptr, ThemeSet32},
    {THEMES, "Ice", ACTION, MAIN, nullptr, ThemeSet33},
    {THEMES, "Lavender", ACTION, MAIN, nullptr, ThemeSet34},
    {THEMES, "Plum", ACTION, MAIN, nullptr, ThemeSet35},
    {SIZES, "Exit Menu Sizes", OPEN, MAIN},
    {SIZES, "Menu Size 50%", ACTION, MAIN, nullptr, SizeSet0},
    {SIZES, "Menu Size 60%", ACTION, MAIN, nullptr, SizeSet1},
    {SIZES, "Menu Size 70%", ACTION, MAIN, nullptr, SizeSet2},
    {SIZES, "Menu Size 80%", ACTION, MAIN, nullptr, SizeSet3},
    {SIZES, "Menu Size 90%", ACTION, MAIN, nullptr, SizeSet4},
    {SIZES, "Menu Size 110%", ACTION, MAIN, nullptr, SizeSet5},
    {SIZES, "Menu Size 125%", ACTION, MAIN, nullptr, SizeSet6},
    {SIZES, "Menu Size 150%", ACTION, MAIN, nullptr, SizeSet7},
    {SIZES, "Menu Size 175%", ACTION, MAIN, nullptr, SizeSet8},
    {SIZES, "Menu Size 200%", ACTION, MAIN, nullptr, SizeSet9},
    {ITEMMULT, "Exit Item Multiplier", OPEN, MAIN},
    {ITEMMULT, "Multiplier Off (x1)", ACTION, MAIN, nullptr, ItemMultOff},
    {ITEMMULT, "Multiplier x2", ACTION, MAIN, nullptr, ItemMult2},
    {ITEMMULT, "Multiplier x3", ACTION, MAIN, nullptr, ItemMult3},
    {ITEMMULT, "Multiplier x4", ACTION, MAIN, nullptr, ItemMult4},
    {ITEMMULT, "Multiplier x5", ACTION, MAIN, nullptr, ItemMult5},
    {ITEMMULT, "Multiplier x6", ACTION, MAIN, nullptr, ItemMult6},
    {ITEMMULT, "Multiplier x8", ACTION, MAIN, nullptr, ItemMult8},
    {ITEMMULT, "Multiplier x10", ACTION, MAIN, nullptr, ItemMult10},
    {ITEMMULT, "Multiplier x12", ACTION, MAIN, nullptr, ItemMult12},
    {ITEMMULT, "Multiplier x16", ACTION, MAIN, nullptr, ItemMult16},
    {ITEMMULT, "Multiplier x20", ACTION, MAIN, nullptr, ItemMult20},
    {SOUNDS, "Exit Soundboard", OPEN, MAIN},
    {SOUNDS, g_playLabel, ACTION, MAIN, nullptr, PlaySelected},
    {SOUNDS, "Next Song", ACTION, MAIN, nullptr, NextSong},
    {SOUNDS, "Previous Song", ACTION, MAIN, nullptr, PrevSong},
    {SOUNDS, "Stop Sound", ACTION, MAIN, nullptr, StopSound},
    {SOUNDS, "Loop", TOGGLE, MAIN, &f_soundLoop, ApplyLoop},
    {SOUNDS, "Everyone Hears It", TOGGLE, MAIN, &f_shareVoice, ApplyShare},
    {SOUNDS, "Hear It Yourself", TOGGLE, MAIN, &f_hearSelf},
    {SOUNDS, "Volume Up", ACTION, MAIN, nullptr, VolUp},
    {SOUNDS, "Volume Down", ACTION, MAIN, nullptr, VolDown},
    {SOUNDS, "Random Sound", ACTION, MAIN, nullptr, RandomSound},
    {SOUNDS, "Play: Twinkle Star", ACTION, MAIN, nullptr, Snd0},
    {SOUNDS, "Play: Mary's Lamb", ACTION, MAIN, nullptr, Snd1},
    {SOUNDS, "Play: Ode to Joy", ACTION, MAIN, nullptr, Snd2},
    {SOUNDS, "Play: Jingle Bells", ACTION, MAIN, nullptr, Snd3},
    {SOUNDS, "Play: Row Your Boat", ACTION, MAIN, nullptr, Snd4},
    {SOUNDS, "Play: Happy Birthday", ACTION, MAIN, nullptr, Snd5},
    {SOUNDS, "Play: Fur Elise", ACTION, MAIN, nullptr, Snd6},
    {SOUNDS, "Play: Scale Run", ACTION, MAIN, nullptr, Snd7},
    {SOUNDS, "Play: Beep", ACTION, MAIN, nullptr, Snd8},
    {SOUNDS, "Play: Double Beep", ACTION, MAIN, nullptr, Snd9},
    {SOUNDS, "Play: Siren", ACTION, MAIN, nullptr, Snd10},
    {SOUNDS, "Play: Laser", ACTION, MAIN, nullptr, Snd11},
    {SOUNDS, "Play: Coin", ACTION, MAIN, nullptr, Snd12},
    {SOUNDS, "Play: Power Up", ACTION, MAIN, nullptr, Snd13},
    {SOUNDS, "Play: Power Down", ACTION, MAIN, nullptr, Snd14},
    {SOUNDS, "Play: Drum Roll", ACTION, MAIN, nullptr, Snd15},
    {SOUNDS, "Play: Alarm", ACTION, MAIN, nullptr, Snd16},
    {SOUNDS, "Play: Doorbell", ACTION, MAIN, nullptr, Snd17},
    {SOUNDS, "Play: Bell", ACTION, MAIN, nullptr, Snd18},
    {SOUNDS, "Play: Whoosh", ACTION, MAIN, nullptr, Snd19},
    {SOUNDS, "Play: Heartbeat", ACTION, MAIN, nullptr, Snd20},
    {SOUNDS, "Play: Wobble", ACTION, MAIN, nullptr, Snd21},
    {SOUNDS, "Play: My Song", ACTION, MAIN, nullptr, Snd22},
    {DECOR, "Exit Decorations", OPEN, MAIN},
    {DECOR, "Border: Off", ACTION, MAIN, nullptr, BorderStyle0},
    {DECOR, "Border: Stripes", ACTION, MAIN, nullptr, BorderStyle1},
    {DECOR, "Border: Solid", ACTION, MAIN, nullptr, BorderStyle2},
    {DECOR, "Border: Dots", ACTION, MAIN, nullptr, BorderStyle3},
    {DECOR, "Border: Lights", ACTION, MAIN, nullptr, BorderStyle4},
    {DECOR, "Border: Zigzag", ACTION, MAIN, nullptr, BorderStyle5},
    {DECOR, "Border: Checker", ACTION, MAIN, nullptr, BorderStyle6},
    {DECOR, "Border Color: Match Theme", ACTION, MAIN, nullptr, BorderColor0},
    {DECOR, "Border Color: Red and White", ACTION, MAIN, nullptr, BorderColor1},
    {DECOR, "Border Color: Orange and Black", ACTION, MAIN, nullptr, BorderColor2},
    {DECOR, "Border Color: Green and Red", ACTION, MAIN, nullptr, BorderColor3},
    {DECOR, "Border Color: Pink and White", ACTION, MAIN, nullptr, BorderColor4},
    {DECOR, "Border Color: Gold and Black", ACTION, MAIN, nullptr, BorderColor5},
    {DECOR, "Border Color: Blue and White", ACTION, MAIN, nullptr, BorderColor6},
    {DECOR, "Border Color: Mint and White", ACTION, MAIN, nullptr, BorderColor7},
    {DECOR, "Border Color: Purple and Gold", ACTION, MAIN, nullptr, BorderColor8},
    {DECOR, "Border Color: Teal and Coral", ACTION, MAIN, nullptr, BorderColor9},
    {DECOR, "Border Color: Black and White", ACTION, MAIN, nullptr, BorderColor10},
    {DECOR, "Border Color: Rainbow", ACTION, MAIN, nullptr, BorderColor11},
    {DECOR, "Candy Canes", TOGGLE, MAIN, &f_deco[0]},
    {DECOR, "Pumpkins", TOGGLE, MAIN, &f_deco[1]},
    {DECOR, "Stars", TOGGLE, MAIN, &f_deco[2]},
    {DECOR, "Hearts", TOGGLE, MAIN, &f_deco[3]},
    {DECOR, "Snowflakes", TOGGLE, MAIN, &f_deco[4]},
    {DECOR, "Gems", TOGGLE, MAIN, &f_deco[5]},
    {DECOR, "Balloons", TOGGLE, MAIN, &f_deco[6]},
    {DECOR, "Ghosts", TOGGLE, MAIN, &f_deco[7]},
    {DECOR, "Flowers", TOGGLE, MAIN, &f_deco[8]},
    {DECOR, "Lights", TOGGLE, MAIN, &f_deco[9]},
    {DECOR, "Snowmen", TOGGLE, MAIN, &f_deco[10]},
    {DECOR, "Crown", TOGGLE, MAIN, &f_deco[11]},
    {WAVS, "Exit My WAVs", OPEN, MAIN},
    {WAVS, g_wavLabel, ACTION, MAIN, nullptr, WavPlay},
    {WAVS, "Next WAV", ACTION, MAIN, nullptr, WavNext},
    {WAVS, "Previous WAV", ACTION, MAIN, nullptr, WavPrev},
    {WAVS, "Refresh Files", ACTION, MAIN, nullptr, WavScan},
    {WAVS, "Stop Sound", ACTION, MAIN, nullptr, StopSound},
    {WAVS, "Loop", TOGGLE, MAIN, &f_soundLoop, ApplyLoop},
    {WAVS, "Everyone Hears It", TOGGLE, MAIN, &f_shareVoice, ApplyShare},
    {WAVS, "Hear It Yourself", TOGGLE, MAIN, &f_hearSelf},
    {WAVS, "Volume Up", ACTION, MAIN, nullptr, VolUp},
    {WAVS, "Volume Down", ACTION, MAIN, nullptr, VolDown},
    {ITEMTEXT, "Exit Item Text", OPEN, MAIN},
    {ITEMTEXT, g_textLabel, ACTION, MAIN, nullptr, TextShow},
    {ITEMTEXT, g_textSpawnLabel, ACTION, MAIN, nullptr, TextSpawnNext},
    {ITEMTEXT, "Restart Text", ACTION, MAIN, nullptr, TextRestart},
    {ITEMTEXT, "Backspace", ACTION, MAIN, nullptr, TextBack},
    {ITEMTEXT, "Space", ACTION, MAIN, nullptr, TextSpace},
    {ITEMTEXT, "Clear Text", ACTION, MAIN, nullptr, TextClear},
    {ITEMTEXT, "Preset: HEARTVRONTOP IS A W", ACTION, MAIN, nullptr, TextPreset},
    {ITEMTEXT, "A", ACTION, MAIN, nullptr, KeyA},
    {ITEMTEXT, "B", ACTION, MAIN, nullptr, KeyB},
    {ITEMTEXT, "C", ACTION, MAIN, nullptr, KeyC},
    {ITEMTEXT, "D", ACTION, MAIN, nullptr, KeyD},
    {ITEMTEXT, "E", ACTION, MAIN, nullptr, KeyE},
    {ITEMTEXT, "F", ACTION, MAIN, nullptr, KeyF},
    {ITEMTEXT, "G", ACTION, MAIN, nullptr, KeyG},
    {ITEMTEXT, "H", ACTION, MAIN, nullptr, KeyH},
    {ITEMTEXT, "I", ACTION, MAIN, nullptr, KeyI},
    {ITEMTEXT, "J", ACTION, MAIN, nullptr, KeyJ},
    {ITEMTEXT, "K", ACTION, MAIN, nullptr, KeyK},
    {ITEMTEXT, "L", ACTION, MAIN, nullptr, KeyL},
    {ITEMTEXT, "M", ACTION, MAIN, nullptr, KeyM},
    {ITEMTEXT, "N", ACTION, MAIN, nullptr, KeyN},
    {ITEMTEXT, "O", ACTION, MAIN, nullptr, KeyO},
    {ITEMTEXT, "P", ACTION, MAIN, nullptr, KeyP},
    {ITEMTEXT, "Q", ACTION, MAIN, nullptr, KeyQ},
    {ITEMTEXT, "R", ACTION, MAIN, nullptr, KeyR},
    {ITEMTEXT, "S", ACTION, MAIN, nullptr, KeyS},
    {ITEMTEXT, "T", ACTION, MAIN, nullptr, KeyT},
    {ITEMTEXT, "U", ACTION, MAIN, nullptr, KeyU},
    {ITEMTEXT, "V", ACTION, MAIN, nullptr, KeyV},
    {ITEMTEXT, "W", ACTION, MAIN, nullptr, KeyW},
    {ITEMTEXT, "X", ACTION, MAIN, nullptr, KeyX},
    {ITEMTEXT, "Y", ACTION, MAIN, nullptr, KeyY},
    {ITEMTEXT, "Z", ACTION, MAIN, nullptr, KeyZ},
    {ITEMTEXT, "0", ACTION, MAIN, nullptr, Key0},
    {ITEMTEXT, "1", ACTION, MAIN, nullptr, Key1},
    {ITEMTEXT, "2", ACTION, MAIN, nullptr, Key2},
    {ITEMTEXT, "3", ACTION, MAIN, nullptr, Key3},
    {ITEMTEXT, "4", ACTION, MAIN, nullptr, Key4},
    {ITEMTEXT, "5", ACTION, MAIN, nullptr, Key5},
    {ITEMTEXT, "6", ACTION, MAIN, nullptr, Key6},
    {ITEMTEXT, "7", ACTION, MAIN, nullptr, Key7},
    {ITEMTEXT, "8", ACTION, MAIN, nullptr, Key8},
    {ITEMTEXT, "9", ACTION, MAIN, nullptr, Key9},
    {SETTINGS, "Exit Settings", OPEN, MAIN},
    {SETTINGS, "Change Pointer Position", ACTION, MAIN, nullptr, NextPointer},
    {SETTINGS, "Iron Man Speed", ACTION, MAIN, nullptr, NextIronSpeed},
    {SETTINGS, "Car Speed", ACTION, MAIN, nullptr, NextCarSpeed},
    {SETTINGS, "Right Hand Menu", TOGGLE, MAIN, &f_rightHand, PointerToHand},
    {SETTINGS, "Change Menu Theme", ACTION, MAIN, nullptr, NextTheme},
    {SETTINGS, "Rainbow Menu", TOGGLE, MAIN, &f_rainbow},
    {SETTINGS, "Show FPS", TOGGLE, MAIN, &f_fps},
    {SETTINGS, "Show Clock", TOGGLE, MAIN, &f_clock},
    {SETTINGS, "24 Hour Clock", TOGGLE, MAIN, &f_clock24},
    {SETTINGS, "Auto Theme", TOGGLE, MAIN, &f_autoTheme},
    {SETTINGS, "Button Buzz", TOGGLE, MAIN, &f_haptics},
    {SETTINGS, "Show Speed", TOGGLE, MAIN, &f_speedText},
    {SETTINGS, "Show Height", TOGGLE, MAIN, &f_heightText},
    {SETTINGS, "Menu Size", ACTION, MAIN, nullptr, NextMenuSize},
    {SETTINGS, "Notifications", TOGGLE, MAIN, &f_notify},
    {SETTINGS, "Turn Everything Off", ACTION, MAIN, nullptr, AllOff},
};
static constexpr int kItems = sizeof(g_items) / sizeof(g_items[0]);
static Item g_returnItem = {ENABLED, "Exit Enabled Mods", OPEN, MAIN};

// Tooltips, shown under the menu while your finger is on a button (like ii's).
static const struct { const char *label, *tip; } kTips[] = {
    {"Settings", "Opens the settings."},
    {"Movement Mods", "Opens the movement mods."},
    {"Enabled Mods", "Shows every mod you have on."},
    {"Platforms", "Hold a grip to stand on a platform."},
    {"Fly", "Hold A to fly where you look."},
    {"Fast Fly", "Makes Fly a lot faster."},
    {"Velocity Fly", "Hold A to shoot where your hand points."},
    {"Car", "Hold A to drive where you look, floating just above the ground."},
    {"Car Speed", "Slow, normal or fast."},
    {"Teleport Gun", "Hold right grip to aim, trigger to teleport."},
    {"Iron Man", "Hold X and A to fly with your hands."},
    {"Noclip", "Hold right trigger to go through walls."},
    {"Speed Boost", "Faster and higher jumps."},
    {"No Gravity", "Turns your gravity off."},
    {"Low Gravity", "Jump higher, fall slower."},
    {"Super Jump", "A much higher jump. Overrides Speed Boost while on."},
    {"Slow Fall", "You never fall faster than a gentle glide."},
    {"Air Jump", "Press Y in the air to jump again."},
    {"Dash", "Press X to dash the way you look."},
    {"Hover", "Hold left trigger to hang in the air (not with a dance picked)."},
    {"Bouncy", "Hard landings bounce you back up."},
    {"Jetpack", "Hold B to thrust upward (needs Right Hand Menu off)."},
    {"Launch Up", "Shoots you straight up."},
    {"Teleport Forward", "Teleports you 5 meters the way you look."},
    {"Teleport Up", "Teleports you 5 meters up."},
    {"Save Spot 2", "Remembers a second spot."},
    {"Go To Spot 2", "Teleports you to your second spot."},
    {"Stop Moving", "Stops all your movement right now."},
    {"Save Spot", "Remembers where you are."},
    {"Go To Spot", "Teleports you to your saved spot."},
    {"Change Menu Theme", "Fire, Orange, Blue, Green, Purple, Dark, Pink, Red, Gold, Cyan, Mint, Sunset."},
    {"Rainbow Menu", "The menu cycles through colors."},
    {"Show FPS", "Frames per second in the title."},
    {"Show Clock", "The time in the title."},
    {"24 Hour Clock", "Show Clock uses 24 hour time."},
    {"Auto Theme", "The menu changes theme every few seconds."},
    {"Button Buzz", "A small buzz when you press a button."},
    {"Show Speed", "Your speed in the title."},
    {"Show Height", "How high you are, in the title."},
    {"Menu Size", "Cycles the menu size: normal, big, huge, small."},
    {"Superman", "Both hands out in front. Hold left trigger."},
    {"Flap", "Arms flap like wings. Hold left trigger."},
    {"Boxing", "Punches, one hand after the other. Hold left trigger."},
    {"Clap", "Hands clap in front of you. Hold left trigger."},
    {"Hands Up", "Both arms up and swaying. Hold left trigger."},
    {"Sprinkler", "One hand sweeps out and back. Hold left trigger."},
    {"Dead", "Both arms hang limp at your sides. Hold left trigger."},
    {"Collapse", "Arms rise, then drop and go limp, over and over. Hold left trigger."},
    {"Corpse", "Arms crossed over your chest, still. Hold left trigger."},
    {"Robot", "Stiff robot poses that snap from one to the next. Hold left trigger."},
    {"Heart", "Hands pulse together in front of your chest. Hold left trigger."},
    {"Flex", "Arms bent, fists by your head. Hold left trigger."},
    {"Shrug", "Hands out low and lifting. Hold left trigger."},
    {"Conductor", "One hand draws a figure eight in the air. Hold left trigger."},
    {"Both Waving", "Both hands up and waving. Hold left trigger."},
    {"Floss", "Arms swing side to side. Hold left trigger."},
    {"Dab", "The classic dab. Hold left trigger."},
    {"Salute", "A salute. Hold left trigger."},
    {"Zombie", "Arms out in front, wobbling. Hold left trigger."},
    {"Lasso", "One hand spins over your head. Hold left trigger."},
    {"Chicken", "Arms flap at your sides. Hold left trigger."},
    {"Line (10)", "10 of the item in your left hand form a line in front of you."},
    {"Spiral", "12 of the item in your left hand form a spiral in front of you."},
    {"Pyramid", "14 of the item in your left hand stack into a pyramid in front of you."},
    {"Heart", "16 of the item in your left hand form a heart in front of you."},
    {"Cross", "9 of the item in your left hand form a plus sign in front of you."},
    {"Star", "10 of the item in your left hand form a star in front of you."},
    {"Donut", "24 of the item in your left hand form a double ring in front of you."},
    {"Diamond", "12 of the item in your left hand form a diamond in front of you."},
    {"Arrow", "9 of the item in your left hand form an arrow pointing away from you."},
    {"Wave", "12 of the item in your left hand form a wavy line in front of you."},
    {"Tower (8)", "8 of the item in your left hand stack up in a column in front of you."},
    {"Wall (5x3)", "15 of the item in your left hand form a wall in front of you."},
    {"Big Grid (5x5)", "25 of the item in your left hand form a big grid in front of you."},
    {"Funny Run", "Arms pump like a sprint. Hold left trigger."},
    {"Funny Run Speed", "Slow, normal or fast arm pumping."},
    {"Spin", "Both hands circle in front of you. Hold left trigger."},
    {"Upside Down", "Hands hang low like you are flipped. Hold left trigger."},
    {"Rig Mods", "Pick a dance, then hold left trigger. Everyone sees it."},
    {"Items", "Spawn copies of the item in your left hand. Everyone sees them."},
    {"Spawn Item Gun", "Hold an item in your left hand. Right grip aims, trigger spawns it."},
    {"Duplicate Items (3x3)", "9 of the item in your left hand appear in front of you."},
    {"Duplicate Items (3x3x3)", "27 of the item in your left hand appear in front of you."},
    {"Duplicate Items (Circle)", "12 of the item in your left hand form a ring in front of you."},
    {"Big Square (4x4)", "16 of the item in your left hand form a square in front of you."},
    {"Big Triangle", "15 of the item in your left hand form a triangle in front of you."},
    {"Clear Item Queue", "Stops the items of your own that are still waiting to spawn."},
    {"Multiplier Off (x1)", "Spawn On Hand makes one item again."},
    {"Soundboard", "Sounds on your own headset. Turn on Everyone Hears It to share them."},
    {"Everyone Hears It", "The room hears the sound through voice chat. One sound every 3 seconds, 60 seconds at most, never louder than 70%. Your mic is off while it plays."},
    {"Hear It Yourself", "Plays the sound on your own headset too."},
    {"Decorations", "Borders and decorations around the menu. Only you see them."},
    {"Next Song", "Picks the next sound in the list."},
    {"Previous Song", "Picks the previous sound in the list."},
    {"Stop Sound", "Stops the sound."},
    {"Loop", "Repeats the sound until you stop it."},
    {"Random Sound", "Picks a sound at random and plays it."},
    {"My WAVs", "Your own .wav files from the sounds folder. Only you hear them."},
    {"Refresh Files", "Looks again in the sounds folder for .wav files."},
    {"Next WAV", "Picks the next .wav file."},
    {"Previous WAV", "Picks the previous .wav file."},
    {"Item Text", "Type a word on the keyboard, then spell it with your left-hand item. One line per press."},
    {"Restart Text", "Start the word again from its first line."},
    {"Spawn On Hand", "Each right trigger spawns the item from your left hand at your right hand."},
    {"Wave", "Waves one hand, the other on your hip. Hold left trigger."},
    {"Helicopter", "Both arms spin around you. Hold left trigger."},
    {"T-Pose", "The classic T-pose. Hold left trigger."},
    {"Windmill", "Arms spin like a windmill. Hold left trigger."},
    {"Swim", "Swims through the air. Hold left trigger."},
    {"Jumping Jacks", "Arms go up and down at your sides. Hold left trigger."},
    {"Spaz", "Hands shake all over the place. Hold left trigger."},
    {"Beyblade", "Hands spin fast around your waist. Hold left trigger."},
    {"Change Pointer Position", "Moves the ball on your finger."},
    {"Iron Man Speed", "Changes how strong Iron Man is."},
    {"Right Hand Menu", "Hold B, menu on your right hand."},
    {"Notifications", "Messages when you switch something."},
    {"Turn Everything Off", "Switches every mod off."},
};
static const char *TipFor(const char *label) {
    if (strncmp(label, "Exit ", 5) == 0) return "Returns you back to the main category.";
    for (auto &t : kTips) if (strcmp(t.label, label) == 0) return t.tip;
    return "";
}

// Colors in the ii menu style: an animated background and darker buttons when on.
struct Theme { const char *name; Color bgA, bgB, button, buttonOn, title; };
static const Theme kThemes[] = {
    {"Fire",   Color(0.90f, 0.12f, 0.04f, 1), Color(1.0f, 0.42f, 0.0f, 1), Color(0.50f, 0.06f, 0.03f, 1), Color(1.0f, 0.45f, 0.05f, 1), Color(1.0f, 0.62f, 0.20f, 1)},
    {"Orange", Color(1.0f, 0.50f, 0.0f, 1), Color(1.0f, 0.40f, 0.0f, 1), Color(0.67f, 0.33f, 0.0f, 1), Color(0.33f, 0.16f, 0.0f, 1), Color(1.0f, 0.75f, 0.49f, 1)},
    {"Blue",   Color(0.20f, 0.50f, 1.0f, 1), Color(0.10f, 0.30f, 0.85f, 1), Color(0.10f, 0.28f, 0.65f, 1), Color(0.04f, 0.12f, 0.32f, 1), Color(0.70f, 0.85f, 1.0f, 1)},
    {"Green",  Color(0.30f, 0.85f, 0.30f, 1), Color(0.15f, 0.65f, 0.20f, 1), Color(0.12f, 0.45f, 0.12f, 1), Color(0.05f, 0.22f, 0.05f, 1), Color(0.75f, 1.0f, 0.75f, 1)},
    {"Purple", Color(0.65f, 0.30f, 1.0f, 1), Color(0.45f, 0.15f, 0.85f, 1), Color(0.38f, 0.15f, 0.60f, 1), Color(0.18f, 0.06f, 0.30f, 1), Color(0.88f, 0.75f, 1.0f, 1)},
    {"Dark",   Color(0.12f, 0.12f, 0.14f, 1), Color(0.05f, 0.05f, 0.06f, 1), Color(0.22f, 0.22f, 0.25f, 1), Color(0.45f, 0.45f, 0.50f, 1), Color(0.85f, 0.85f, 0.90f, 1)},
    {"Pink",   Color(1.0f, 0.40f, 0.70f, 1), Color(0.90f, 0.20f, 0.55f, 1), Color(0.60f, 0.15f, 0.38f, 1), Color(0.30f, 0.05f, 0.18f, 1), Color(1.0f, 0.80f, 0.90f, 1)},
    {"Red",    Color(0.85f, 0.10f, 0.10f, 1), Color(0.60f, 0.05f, 0.05f, 1), Color(0.45f, 0.05f, 0.05f, 1), Color(0.22f, 0.02f, 0.02f, 1), Color(1.0f, 0.70f, 0.70f, 1)},
    {"Gold",   Color(1.0f, 0.82f, 0.20f, 1), Color(0.85f, 0.65f, 0.05f, 1), Color(0.60f, 0.45f, 0.05f, 1), Color(0.30f, 0.22f, 0.02f, 1), Color(1.0f, 0.95f, 0.70f, 1)},
    {"Cyan",   Color(0.10f, 0.85f, 0.90f, 1), Color(0.05f, 0.60f, 0.75f, 1), Color(0.05f, 0.40f, 0.50f, 1), Color(0.02f, 0.20f, 0.26f, 1), Color(0.75f, 1.0f, 1.0f, 1)},
    {"Mint",   Color(0.50f, 1.0f, 0.80f, 1), Color(0.30f, 0.85f, 0.65f, 1), Color(0.18f, 0.55f, 0.42f, 1), Color(0.08f, 0.28f, 0.21f, 1), Color(0.85f, 1.0f, 0.95f, 1)},
    {"Sunset", Color(1.0f, 0.35f, 0.35f, 1), Color(0.60f, 0.25f, 0.85f, 1), Color(0.50f, 0.15f, 0.40f, 1), Color(0.25f, 0.07f, 0.22f, 1), Color(1.0f, 0.80f, 0.60f, 1)},
    {"Ruby", Color(0.95f, 0.24f, 0.24f, 1), Color(0.75f, 0.20f, 0.07f, 1), Color(0.45f, 0.09f, 0.09f, 1), Color(0.22f, 0.03f, 0.03f, 1), Color(1.00f, 0.65f, 0.65f, 1)},
    {"Scarlet", Color(0.95f, 0.36f, 0.24f, 1), Color(0.75f, 0.31f, 0.07f, 1), Color(0.45f, 0.15f, 0.09f, 1), Color(0.22f, 0.06f, 0.03f, 1), Color(1.00f, 0.71f, 0.65f, 1)},
    {"Coral", Color(0.95f, 0.47f, 0.24f, 1), Color(0.75f, 0.42f, 0.07f, 1), Color(0.45f, 0.21f, 0.09f, 1), Color(0.22f, 0.10f, 0.03f, 1), Color(1.00f, 0.77f, 0.65f, 1)},
    {"Ember", Color(0.95f, 0.59f, 0.24f, 1), Color(0.75f, 0.53f, 0.07f, 1), Color(0.45f, 0.27f, 0.09f, 1), Color(0.22f, 0.13f, 0.03f, 1), Color(1.00f, 0.82f, 0.65f, 1)},
    {"Amber", Color(0.95f, 0.71f, 0.24f, 1), Color(0.75f, 0.65f, 0.07f, 1), Color(0.45f, 0.33f, 0.09f, 1), Color(0.22f, 0.16f, 0.03f, 1), Color(1.00f, 0.88f, 0.65f, 1)},
    {"Honey", Color(0.95f, 0.83f, 0.24f, 1), Color(0.74f, 0.75f, 0.07f, 1), Color(0.45f, 0.39f, 0.09f, 1), Color(0.22f, 0.19f, 0.03f, 1), Color(1.00f, 0.94f, 0.65f, 1)},
    {"Lemon", Color(0.95f, 0.95f, 0.24f, 1), Color(0.63f, 0.75f, 0.07f, 1), Color(0.45f, 0.45f, 0.09f, 1), Color(0.22f, 0.22f, 0.03f, 1), Color(1.00f, 1.00f, 0.65f, 1)},
    {"Lime", Color(0.83f, 0.95f, 0.24f, 1), Color(0.52f, 0.75f, 0.07f, 1), Color(0.39f, 0.45f, 0.09f, 1), Color(0.19f, 0.22f, 0.03f, 1), Color(0.94f, 1.00f, 0.65f, 1)},
    {"Fern", Color(0.71f, 0.95f, 0.24f, 1), Color(0.40f, 0.75f, 0.07f, 1), Color(0.33f, 0.45f, 0.09f, 1), Color(0.16f, 0.22f, 0.03f, 1), Color(0.88f, 1.00f, 0.65f, 1)},
    {"Jade", Color(0.59f, 0.95f, 0.24f, 1), Color(0.29f, 0.75f, 0.07f, 1), Color(0.27f, 0.45f, 0.09f, 1), Color(0.13f, 0.22f, 0.03f, 1), Color(0.82f, 1.00f, 0.65f, 1)},
    {"Emerald", Color(0.47f, 0.95f, 0.24f, 1), Color(0.18f, 0.75f, 0.07f, 1), Color(0.21f, 0.45f, 0.09f, 1), Color(0.10f, 0.22f, 0.03f, 1), Color(0.77f, 1.00f, 0.65f, 1)},
    {"Spring", Color(0.36f, 0.95f, 0.24f, 1), Color(0.07f, 0.75f, 0.08f, 1), Color(0.15f, 0.45f, 0.09f, 1), Color(0.06f, 0.22f, 0.03f, 1), Color(0.71f, 1.00f, 0.65f, 1)},
    {"Teal", Color(0.24f, 0.95f, 0.24f, 1), Color(0.07f, 0.75f, 0.20f, 1), Color(0.09f, 0.45f, 0.09f, 1), Color(0.03f, 0.22f, 0.03f, 1), Color(0.65f, 1.00f, 0.65f, 1)},
    {"Aqua", Color(0.24f, 0.95f, 0.36f, 1), Color(0.07f, 0.75f, 0.31f, 1), Color(0.09f, 0.45f, 0.15f, 1), Color(0.03f, 0.22f, 0.06f, 1), Color(0.65f, 1.00f, 0.71f, 1)},
    {"Azure", Color(0.24f, 0.95f, 0.48f, 1), Color(0.07f, 0.75f, 0.42f, 1), Color(0.09f, 0.45f, 0.21f, 1), Color(0.03f, 0.22f, 0.10f, 1), Color(0.65f, 1.00f, 0.77f, 1)},
    {"Sky", Color(0.24f, 0.95f, 0.59f, 1), Color(0.07f, 0.75f, 0.53f, 1), Color(0.09f, 0.45f, 0.27f, 1), Color(0.03f, 0.22f, 0.13f, 1), Color(0.65f, 1.00f, 0.82f, 1)},
    {"Cobalt", Color(0.24f, 0.95f, 0.71f, 1), Color(0.07f, 0.75f, 0.65f, 1), Color(0.09f, 0.45f, 0.33f, 1), Color(0.03f, 0.22f, 0.16f, 1), Color(0.65f, 1.00f, 0.88f, 1)},
    {"Sapphire", Color(0.24f, 0.95f, 0.83f, 1), Color(0.07f, 0.74f, 0.75f, 1), Color(0.09f, 0.45f, 0.39f, 1), Color(0.03f, 0.22f, 0.19f, 1), Color(0.65f, 1.00f, 0.94f, 1)},
    {"Indigo", Color(0.24f, 0.95f, 0.95f, 1), Color(0.07f, 0.63f, 0.75f, 1), Color(0.09f, 0.45f, 0.45f, 1), Color(0.03f, 0.22f, 0.22f, 1), Color(0.65f, 1.00f, 1.00f, 1)},
    {"Violet", Color(0.24f, 0.83f, 0.95f, 1), Color(0.07f, 0.52f, 0.75f, 1), Color(0.09f, 0.39f, 0.45f, 1), Color(0.03f, 0.19f, 0.22f, 1), Color(0.65f, 0.94f, 1.00f, 1)},
    {"Grape", Color(0.24f, 0.71f, 0.95f, 1), Color(0.07f, 0.40f, 0.75f, 1), Color(0.09f, 0.33f, 0.45f, 1), Color(0.03f, 0.16f, 0.22f, 1), Color(0.65f, 0.88f, 1.00f, 1)},
    {"Orchid", Color(0.24f, 0.59f, 0.95f, 1), Color(0.07f, 0.29f, 0.75f, 1), Color(0.09f, 0.27f, 0.45f, 1), Color(0.03f, 0.13f, 0.22f, 1), Color(0.65f, 0.82f, 1.00f, 1)},
    {"Magenta", Color(0.24f, 0.47f, 0.95f, 1), Color(0.07f, 0.18f, 0.75f, 1), Color(0.09f, 0.21f, 0.45f, 1), Color(0.03f, 0.10f, 0.22f, 1), Color(0.65f, 0.77f, 1.00f, 1)},
    {"Fuchsia", Color(0.24f, 0.36f, 0.95f, 1), Color(0.08f, 0.07f, 0.75f, 1), Color(0.09f, 0.15f, 0.45f, 1), Color(0.03f, 0.06f, 0.22f, 1), Color(0.65f, 0.71f, 1.00f, 1)},
    {"Rose", Color(0.24f, 0.24f, 0.95f, 1), Color(0.20f, 0.07f, 0.75f, 1), Color(0.09f, 0.09f, 0.45f, 1), Color(0.03f, 0.03f, 0.22f, 1), Color(0.65f, 0.65f, 1.00f, 1)},
    {"Blush", Color(0.36f, 0.24f, 0.95f, 1), Color(0.31f, 0.07f, 0.75f, 1), Color(0.15f, 0.09f, 0.45f, 1), Color(0.06f, 0.03f, 0.22f, 1), Color(0.71f, 0.65f, 1.00f, 1)},
    {"Cherry", Color(0.47f, 0.24f, 0.95f, 1), Color(0.42f, 0.07f, 0.75f, 1), Color(0.21f, 0.09f, 0.45f, 1), Color(0.10f, 0.03f, 0.22f, 1), Color(0.77f, 0.65f, 1.00f, 1)},
    {"Cranberry", Color(0.59f, 0.24f, 0.95f, 1), Color(0.53f, 0.07f, 0.75f, 1), Color(0.27f, 0.09f, 0.45f, 1), Color(0.13f, 0.03f, 0.22f, 1), Color(0.82f, 0.65f, 1.00f, 1)},
    {"Peach", Color(0.71f, 0.24f, 0.95f, 1), Color(0.65f, 0.07f, 0.75f, 1), Color(0.33f, 0.09f, 0.45f, 1), Color(0.16f, 0.03f, 0.22f, 1), Color(0.88f, 0.65f, 1.00f, 1)},
    {"Mango", Color(0.83f, 0.24f, 0.95f, 1), Color(0.75f, 0.07f, 0.74f, 1), Color(0.39f, 0.09f, 0.45f, 1), Color(0.19f, 0.03f, 0.22f, 1), Color(0.94f, 0.65f, 1.00f, 1)},
    {"Moss", Color(0.95f, 0.24f, 0.95f, 1), Color(0.75f, 0.07f, 0.63f, 1), Color(0.45f, 0.09f, 0.45f, 1), Color(0.22f, 0.03f, 0.22f, 1), Color(1.00f, 0.65f, 1.00f, 1)},
    {"Pine", Color(0.95f, 0.24f, 0.83f, 1), Color(0.75f, 0.07f, 0.52f, 1), Color(0.45f, 0.09f, 0.39f, 1), Color(0.22f, 0.03f, 0.19f, 1), Color(1.00f, 0.65f, 0.94f, 1)},
    {"Ocean", Color(0.95f, 0.24f, 0.71f, 1), Color(0.75f, 0.07f, 0.40f, 1), Color(0.45f, 0.09f, 0.33f, 1), Color(0.22f, 0.03f, 0.16f, 1), Color(1.00f, 0.65f, 0.88f, 1)},
    {"Ice", Color(0.95f, 0.24f, 0.59f, 1), Color(0.75f, 0.07f, 0.29f, 1), Color(0.45f, 0.09f, 0.27f, 1), Color(0.22f, 0.03f, 0.13f, 1), Color(1.00f, 0.65f, 0.82f, 1)},
    {"Lavender", Color(0.95f, 0.24f, 0.48f, 1), Color(0.75f, 0.07f, 0.18f, 1), Color(0.45f, 0.09f, 0.21f, 1), Color(0.22f, 0.03f, 0.10f, 1), Color(1.00f, 0.65f, 0.77f, 1)},
    {"Plum", Color(0.95f, 0.24f, 0.36f, 1), Color(0.75f, 0.08f, 0.07f, 1), Color(0.45f, 0.09f, 0.15f, 1), Color(0.22f, 0.03f, 0.06f, 1), Color(1.00f, 0.65f, 0.71f, 1)},
};
static constexpr int kThemeCount = sizeof(kThemes) / sizeof(kThemes[0]);
static int g_theme = 0;
static const Theme &T() { return kThemes[g_theme]; }

// ---- menu objects ----------------------------------------------------------------

// Layout in meters, matching ii's menu as it was ported to Yeeps. Everything is drawn
// on a "board" object: +x goes across, +y goes up, the player looks at it from -z.
static constexpr int kRows = 8;                                   // buttons per page
static const float kBoardW = 0.30f, kBoardH = 0.3825f, kBtnW = 0.27f, kBtnH = 0.0306f;
static const float kTitleY = 0.235f, kSideX = 0.2f, kSideW = 0.06f, kSideH = 0.344f;
static float RowY(int r) { return 0.13f - r * 0.04f; }

static Ref g_menu, g_board, g_bg, g_outline, g_pointer, g_title, g_titleBar;
static Ref g_bgEx[6], g_outEx[6];                  // the other pieces of the rounded panel
static Ref g_rowCube[kRows], g_rowLabel[kRows], g_rowText[kRows];
static Ref g_prev, g_next, g_notifyGo, g_notifyText, g_tipText, g_leftPlatform, g_rightPlatform;
static double g_openTime = 0, g_flashUntil = 0;
static int g_flashRow = -2, g_hover = -2;
static bool g_built = false, g_menuShown = false, g_dirty = true;
static Cat g_cat = MAIN;
static int g_page = 0;
static std::vector<Item *> g_view;                                // what the current page list shows
static double g_time = 0, g_nextPress = 0, g_notifyUntil = 0, g_nextTitle = 0;
static int g_touching = -2, g_frames = 0;
static float g_fps = 0;

static int PageCount() { return g_view.empty() ? 1 : (int)(g_view.size() + kRows - 1) / kRows; }

static void BuildView() {
    g_view.clear();
    if (g_cat == ENABLED) {
        g_view.push_back(&g_returnItem);
        for (auto &it : g_items) if (it.kind == TOGGLE && *it.flag && it.flag != &f_notify) g_view.push_back(&it);
    } else {
        for (auto &it : g_items) if (it.cat == g_cat) g_view.push_back(&it);
    }
    if (g_page >= PageCount()) g_page = 0;
}

static Il2CppObject *Label(Il2CppObject *canvasT, const char *s, Vector3 pos, float w, float h, int size, Ref *textOut) {
    if (!G.font.get() || !G.tText) return nullptr;
    Il2CppObject *go = Class("UnityEngine", "GameObject").CreateNewObjectParameters(CreateMonoString("Label"));
    Il2CppObject *text = go ? G.goAddComponent[go](G.tText) : nullptr;
    if (!text) return nullptr;
    Il2CppObject *t = Tr(go);
    G.trSetParent[t](canvasT, false);
    G.rtSetSizeDelta[t](Vector2(w * 1000, h * 1000));
    G.trSetLocalPosition[t](pos * 1000);
    G.trSetLocalRotation[t](Quaternion(0, 0, 0, 1));
    G.trSetLocalScale[t](Vector3(1, 1, 1));
    G.textSetFont[text](G.font.get());
    G.textSetText[text](CreateMonoString(s));
    G.textSetFontSize[text](size);
    G.textSetAlignment[text](4);                 // MiddleCenter
    if (G.textSetFontStyle.IsValid()) G.textSetFontStyle[text](2);   // Italic, like ii's menu
    if (G.textSetBestFit.IsValid()) {            // shrink long labels to fit their button
        G.textSetBestFit[text](true);
        G.textSetMinSize[text](1);
        G.textSetMaxSize[text](size);
    }
    G.graphicSetColor[text](Color(1, 1, 1, 1));
    if (textOut) textOut->set(text);
    return go;
}

// A world-space canvas (1 unit = 1 mm) under parent.
static Il2CppObject *Canvas(Il2CppObject *parentT, Vector3 pos, float w, float h) {
    Il2CppObject *go = Class("UnityEngine", "GameObject").CreateNewObjectParameters(CreateMonoString("StarterMenuText"));
    Il2CppObject *canvas = go && G.tCanvas ? G.goAddComponent[go](G.tCanvas) : nullptr;
    if (!canvas) return nullptr;
    G.canvasSetRenderMode[canvas](2);            // WorldSpace
    Il2CppObject *ct = Tr(go);
    G.trSetParent[ct](parentT, false);
    G.rtSetSizeDelta[ct](Vector2(w * 1000, h * 1000));
    G.trSetLocalPosition[ct](pos);
    G.trSetLocalRotation[ct](Quaternion(0, 0, 0, 1));
    G.trSetLocalScale[ct](Vector3(0.001f, 0.001f, 0.001f));
    return go;
}

static Il2CppObject *Part(Il2CppObject *parentT, Color c, Vector3 pos, Vector3 scale) {
    Il2CppObject *o = Cube(c, false);
    if (!o) return nullptr;
    Il2CppObject *t = Tr(o);
    G.trSetParent[t](parentT, false);
    G.trSetLocalPosition[t](pos);
    G.trSetLocalScale[t](scale);
    return o;
}

// A flat round disc facing the player: a Unity cylinder turned onto its side.
// w x h gives an ellipse. +x goes across the board, +y up, z is the thickness.
static Il2CppObject *Disc(Il2CppObject *parentT, Color c, Vector3 pos, float w, float h, float thick) {
    Il2CppObject *o = Prim(2, c, false);         // 2 = Cylinder (its height is 2, so half the thickness)
    if (!o) return nullptr;
    Il2CppObject *t = Tr(o);
    G.trSetParent[t](parentT, false);
    G.trSetLocalPosition[t](pos);
    G.trSetLocalRotation[t](Quaternion(0.70710678f, 0, 0, 0.70710678f));   // axis along the board's z
    G.trSetLocalScale[t](Vector3(w, thick * 0.5f, h));
    return o;
}

// A rectangle with round corners, built from 3 boxes and 4 discs (cylinders on their side).
// w x h is the size and rad the corner radius. `main` gets the middle box and the six other
// pieces go into extra[0..5], so SetPanelColor() can paint all of them.
static void RoundRect(Il2CppObject *parentT, Color c, Vector3 center, float w, float h, float rad, float thick,
                      Ref &main, Ref *extra) {
    float ix = w * 0.5f - rad, iy = h * 0.5f - rad;      // where the four corner circles sit
    main.set(Part(parentT, c, center, Vector3(2 * ix, h, thick)));                                    // tall middle
    extra[0].set(Part(parentT, c, center + Vector3(-(ix + rad * 0.5f), 0, 0), Vector3(rad, 2 * iy, thick)));  // left side
    extra[1].set(Part(parentT, c, center + Vector3(ix + rad * 0.5f, 0, 0), Vector3(rad, 2 * iy, thick)));     // right side
    int k = 2;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
            extra[k++].set(Disc(parentT, c, center + Vector3(sx * ix, sy * iy, 0), 2 * rad, 2 * rad, thick));
}
static void SetPanelColor(Ref &main, Ref *extra, Color c) {
    SetColor(main.get(), c);
    for (int i = 0; i < 6; i++) SetColor(extra[i].get(), c);
}


// ---- Decorations around the menu. Local only: nobody else sees them. ----
// Built from simple Unity shapes, as children of the board, and rebuilt only when a choice changes.
static Ref g_decor[170];
static int g_decorN = 0;
static unsigned g_decorSig = 0xFFFFFFFFu;
static void ClearDecor() {
    for (int i = 0; i < g_decorN; i++) Kill(g_decor[i]);
    g_decorN = 0;
    g_decorSig = 0xFFFFFFFFu;
}
static void Keep(Il2CppObject *o) {
    if (!o) return;
    if (g_decorN < 170) g_decor[g_decorN++].set(o);
    else if (Alive(o)) G.destroy(o);
}
static Il2CppObject *Ball(Il2CppObject *parentT, Color c, Vector3 pos, Vector3 scale) {
    Il2CppObject *o = Prim(0, c, false);         // 0 = Sphere
    if (!o) return nullptr;
    Il2CppObject *t = Tr(o);
    G.trSetParent[t](parentT, false);
    G.trSetLocalPosition[t](pos);
    G.trSetLocalScale[t](scale);
    return o;
}
static Il2CppObject *TiltCube(Il2CppObject *parentT, Color c, Vector3 pos, Vector3 scale, float deg) {
    Il2CppObject *o = Part(parentT, c, pos, scale);
    if (!o) return nullptr;
    float h = deg * 0.5f * 0.0174532925f;
    G.trSetLocalRotation[Tr(o)](Quaternion(0, 0, sinf(h), cosf(h)));      // turned around the board's z axis
    return o;
}
static Color Hue(float h) {
    h -= floorf(h);
    float r = fabsf(h * 6 - 3) - 1, g = 2 - fabsf(h * 6 - 2), b = 2 - fabsf(h * 6 - 4);
    return Color(fminf(1.0f, fmaxf(0.0f, r)), fminf(1.0f, fmaxf(0.0f, g)), fminf(1.0f, fmaxf(0.0f, b)), 1);
}
struct BorderPair { float r1, g1, b1, r2, g2, b2; };
static const BorderPair kBorderPairs[12] = {      // 0 follows the theme and 11 is a rainbow, so those rows are unused
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    {1.0f, 0.1f, 0.1f, 1.0f, 1.0f, 1.0f},
    {1.0f, 0.5f, 0.05f, 0.1f, 0.05f, 0.12f},
    {0.1f, 0.7f, 0.2f, 0.9f, 0.1f, 0.1f},
    {1.0f, 0.45f, 0.75f, 1.0f, 1.0f, 1.0f},
    {1.0f, 0.8f, 0.2f, 0.08f, 0.08f, 0.08f},
    {0.2f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f},
    {0.4f, 1.0f, 0.8f, 1.0f, 1.0f, 1.0f},
    {0.55f, 0.2f, 0.85f, 1.0f, 0.8f, 0.2f},
    {0.1f, 0.7f, 0.7f, 1.0f, 0.45f, 0.4f},
    {0.08f, 0.08f, 0.08f, 1.0f, 1.0f, 1.0f},
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
};
static void BuildDecor() {
    ClearDecor();
    Il2CppObject *board = g_board.get();
    if (!board || !Alive(board)) return;
    Il2CppObject *bt = Tr(board);
    const Theme &th = T();
    Color c1 = th.title, c2 = th.button;                   // the two border colors: the theme's by default
    if (g_borderColor >= 1 && g_borderColor <= 10) {
        const BorderPair &bp = kBorderPairs[g_borderColor];
        c1 = Color(bp.r1, bp.g1, bp.b1, 1);
        c2 = Color(bp.r2, bp.g2, bp.b2, 1);
    }
    const bool rainbow = g_borderColor == 11;
    const float zz = -0.003f;
    // the border: little pieces all the way around the panel
    if (g_borderStyle > 0) {
        const float hw = kBoardW * 0.5f + 0.012f, hh = kBoardH * 0.5f + 0.012f;
        int seg = 0;
        auto piece = [&](float x, float y, float w, float h, float ox, float oy) {
            Color c = rainbow ? Hue(seg * 0.037f) : (g_borderStyle == 2 ? c1 : ((seg & 1) ? c2 : c1));
            bool odd = (seg & 1) != 0;
            float push = (g_borderStyle == 5 && odd) ? 0.006f : 0.0f;                    // zigzag: every other one sticks out
            if (g_borderStyle == 3 || g_borderStyle == 4) {                              // dots and lights: little balls
                float d = g_borderStyle == 3 ? 0.012f : 0.016f;
                Keep(Ball(bt, c, Vector3(x, y, zz), Vector3(d, d, d)));
            } else {
                float k = (g_borderStyle == 6 && odd) ? 0.55f : 1.0f;                    // checker: every other one is thinner
                Keep(Part(bt, c, Vector3(x + ox * push, y + oy * push, -0.002f),
                          Vector3(w * (ox != 0 ? k : 1.0f), h * (oy != 0 ? k : 1.0f), 0.006f)));
            }
            seg++;
        };
        const int nx = 9, ny = 12;
        const float spanX = kBoardW - 0.04f, spanY = kBoardH - 0.04f, sx = spanX / nx, sy = spanY / ny;
        for (int i = 0; i < nx; i++) piece(-spanX * 0.5f + (i + 0.5f) * sx, hh, sx * 0.96f, 0.012f, 0, 1);
        for (int i = 0; i < ny; i++) piece(hw, spanY * 0.5f - (i + 0.5f) * sy, 0.012f, sy * 0.96f, 1, 0);
        for (int i = 0; i < nx; i++) piece(spanX * 0.5f - (i + 0.5f) * sx, -hh, sx * 0.96f, 0.012f, 0, -1);
        for (int i = 0; i < ny; i++) piece(-hw, -spanY * 0.5f + (i + 0.5f) * sy, 0.012f, sy * 0.96f, -1, 0);
        piece(hw, hh, 0.016f, 0.016f, 0, 0);
        piece(hw, -hh, 0.016f, 0.016f, 0, 0);
        piece(-hw, -hh, 0.016f, 0.016f, 0, 0);
        piece(-hw, hh, 0.016f, 0.016f, 0, 0);
    }
    const Color white(1, 1, 1, 1);
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        float sg = (float)sgn;
        if (f_deco[0]) {                                   // Candy Canes: standing at the top corners, in the border colors
            float x = sg * 0.19f;
            for (int i = 0; i < 8; i++)
                Keep(Part(bt, rainbow ? Hue(i * 0.12f) : ((i & 1) ? c2 : c1), Vector3(x, 0.196f + i * 0.0125f, zz), Vector3(0.011f, 0.0125f, 0.008f)));
            Keep(Part(bt, c1, Vector3(x - sg * 0.007f, 0.2985f, zz), Vector3(0.026f, 0.011f, 0.008f)));
            Keep(Ball(bt, c2, Vector3(x - sg * 0.02f, 0.292f, zz), Vector3(0.014f, 0.014f, 0.01f)));
        }
        if (f_deco[1]) {                                   // Pumpkins: bottom corners
            Keep(Ball(bt, Color(1.0f, 0.5f, 0.08f, 1), Vector3(sg * 0.17f, -0.215f, zz), Vector3(0.058f, 0.05f, 0.03f)));
            Keep(Part(bt, Color(0.2f, 0.6f, 0.25f, 1), Vector3(sg * 0.17f, -0.188f, zz), Vector3(0.008f, 0.014f, 0.01f)));
        }
        if (f_deco[2])                                     // Stars
            Keep(TiltCube(bt, Color(1.0f, 0.9f, 0.2f, 1), Vector3(sg * 0.265f, 0.10f, zz), Vector3(0.026f, 0.026f, 0.006f), 45));
        if (f_deco[3]) {                                   // Hearts
            Color pink(1.0f, 0.35f, 0.6f, 1);
            float x = sg * 0.265f;
            Keep(Ball(bt, pink, Vector3(x - 0.008f, 0.007f, zz), Vector3(0.017f, 0.017f, 0.008f)));
            Keep(Ball(bt, pink, Vector3(x + 0.008f, 0.007f, zz), Vector3(0.017f, 0.017f, 0.008f)));
            Keep(TiltCube(bt, pink, Vector3(x, -0.003f, zz), Vector3(0.0145f, 0.0145f, 0.008f), 45));
        }
        if (f_deco[4])                                     // Snowflakes: three thin bars crossing
            for (int k = 0; k < 3; k++)
                Keep(TiltCube(bt, white, Vector3(sg * 0.265f, 0.19f, zz), Vector3(0.032f, 0.005f, 0.005f), k * 60.0f));
        if (f_deco[5]) {                                   // Gems: two turned squares in the border colors
            Keep(TiltCube(bt, c1, Vector3(sg * 0.265f, -0.10f, zz), Vector3(0.028f, 0.028f, 0.006f), 45));
            Keep(TiltCube(bt, c2, Vector3(sg * 0.265f, -0.10f, zz - 0.001f), Vector3(0.016f, 0.016f, 0.006f), 45));
        }
        if (f_deco[6]) {                                   // Balloons
            Keep(Ball(bt, rainbow ? Hue(0.1f) : c1, Vector3(sg * 0.17f, 0.35f, zz), Vector3(0.05f, 0.06f, 0.03f)));
            Keep(Part(bt, white, Vector3(sg * 0.17f, 0.31f, zz), Vector3(0.002f, 0.05f, 0.002f)));
        }
        if (f_deco[7]) {                                   // Ghosts
            Keep(Ball(bt, white, Vector3(sg * 0.08f, 0.31f, zz), Vector3(0.045f, 0.05f, 0.025f)));
            Keep(Ball(bt, Color(0.1f, 0.1f, 0.15f, 1), Vector3(sg * 0.08f - 0.01f, 0.318f, zz - 0.004f), Vector3(0.008f, 0.01f, 0.005f)));
            Keep(Ball(bt, Color(0.1f, 0.1f, 0.15f, 1), Vector3(sg * 0.08f + 0.01f, 0.318f, zz - 0.004f), Vector3(0.008f, 0.01f, 0.005f)));
        }
        if (f_deco[8]) {                                   // Flowers: a yellow middle and four pink petals
            float x = sg * 0.265f, y = -0.19f;
            Color petal(1.0f, 0.45f, 0.7f, 1);
            Keep(Ball(bt, Color(1.0f, 0.85f, 0.1f, 1), Vector3(x, y, zz - 0.002f), Vector3(0.014f, 0.014f, 0.008f)));
            Keep(Ball(bt, petal, Vector3(x + 0.014f, y, zz), Vector3(0.014f, 0.014f, 0.008f)));
            Keep(Ball(bt, petal, Vector3(x - 0.014f, y, zz), Vector3(0.014f, 0.014f, 0.008f)));
            Keep(Ball(bt, petal, Vector3(x, y + 0.014f, zz), Vector3(0.014f, 0.014f, 0.008f)));
            Keep(Ball(bt, petal, Vector3(x, y - 0.014f, zz), Vector3(0.014f, 0.014f, 0.008f)));
        }
        if (f_deco[10]) {                                  // Snowmen: a body, a head and a black hat
            float x = sg * 0.265f, y = -0.27f;
            Keep(Ball(bt, white, Vector3(x, y - 0.012f, zz), Vector3(0.05f, 0.05f, 0.03f)));
            Keep(Ball(bt, white, Vector3(x, y + 0.02f, zz), Vector3(0.034f, 0.034f, 0.025f)));
            Keep(Part(bt, Color(0.08f, 0.08f, 0.1f, 1), Vector3(x, y + 0.043f, zz), Vector3(0.03f, 0.012f, 0.012f)));
        }
    }
    if (f_deco[9])                                         // Lights: a string of colored balls under the menu
        for (int i = 0; i < 9; i++)
            Keep(Ball(bt, Hue(i / 9.0f), Vector3(-0.13f + i * 0.0325f, -(kBoardH * 0.5f + 0.034f), zz), Vector3(0.018f, 0.018f, 0.012f)));
    if (f_deco[11]) {                                      // Crown: five gold points above the title
        Color gold(1.0f, 0.82f, 0.2f, 1);
        Keep(Part(bt, gold, Vector3(0, 0.265f, zz), Vector3(0.1f, 0.012f, 0.008f)));
        for (int i = 0; i < 5; i++)
            Keep(Part(bt, gold, Vector3(-0.04f + i * 0.02f, 0.2815f + ((i & 1) ? 0.0f : 0.008f), zz), Vector3(0.012f, 0.022f + ((i & 1) ? 0.0f : 0.016f), 0.008f)));
    }
}
static void UpdateDecor() {
    unsigned mask = 0;
    for (int i = 0; i < 12; i++) if (f_deco[i]) mask |= 1u << i;
    unsigned sig = mask * 2654435761u ^ (unsigned)(g_borderStyle * 97 + g_borderColor * 1013);
    if (g_borderColor == 0) sig ^= (unsigned)g_theme * 7919u;                // only the theme colors change with the theme
    sig ^= (unsigned)(uintptr_t)g_board.get() * 2246822519u;                 // a new board gets new decorations
    if (sig == g_decorSig) return;
    BuildDecor();
    g_decorSig = sig;
}

// Where the ball sits on your hand (ii's four pointer positions).
static const Vector3 kPointerSpots[] = {Vector3(0.01f, -0.017f, 0.19f), Vector3(0, -0.1f, 0.05f),
                                        Vector3(0, 0.1f, -0.05f), Vector3(0, 0.0666f, 0.1f)};
static int g_pointerSpot = 0;

static void PointerToHand() {
    Il2CppObject *p = g_pointer.get(), *hand = f_rightHand ? g_leftHand.get() : g_rightHand.get();
    if (!Alive(p) || !hand) return;
    Il2CppObject *t = Tr(p);
    Vector3 at = kPointerSpots[g_pointerSpot];
    if (f_rightHand) at.x = -at.x;
    G.trSetParent[t](hand, false);
    G.trSetLocalPosition[t](at);
}
static void NextPointer() { g_pointerSpot = (g_pointerSpot + 1) % 4; PointerToHand(); Notify("Pointer position %d", g_pointerSpot + 1); }

// Built once per room; afterwards only shown/hidden, moved and relabeled.
static void BuildMenu() {
    auto NewObject = [](const char *name) {
        return Class("UnityEngine", "GameObject").CreateNewObjectParameters(CreateMonoString(name));
    };
    Il2CppObject *menu = NewObject("T3wtr4fvrMenu"), *board = NewObject("T3wtr4fvrBoard");
    if (!menu || !board) return;
    g_menu.set(menu);
    g_board.set(board);
    Il2CppObject *bt = Tr(board);
    G.trSetParent[bt](Tr(menu), false);
    // Like ii's menu: the board stands 10 cm out from the hand on its +x side, facing +x.
    G.trSetLocalPosition[bt](Vector3(0.1f, 0, 0));
    G.trSetLocalRotation[bt](Quaternion::LookRotation(Vector3(-1, 0, 0), Vector3(0, 0, 1)));

    RoundRect(bt, T().title, Vector3(0, 0, 0.004f), kBoardW + 0.008f, kBoardH + 0.008f, 0.039f, 0.004f, g_outline, g_outEx);
    RoundRect(bt, T().bgA, Vector3(0, 0, 0), kBoardW, kBoardH, 0.035f, 0.01f, g_bg, g_bgEx);
    for (int r = 0; r < kRows; r++)
        g_rowCube[r].set(Part(bt, T().button, Vector3(0, RowY(r), -0.005f), Vector3(kBtnW, kBtnH, 0.009f)));
    g_prev.set(Part(bt, T().button, Vector3(-kSideX, 0, 0), Vector3(kSideW, kSideH, 0.009f)));
    // The title sits on its own dark bar above the board (where ii's Disconnect button was)
    g_titleBar.set(Part(bt, T().buttonOn, Vector3(0, kTitleY, -0.002f), Vector3(kBtnW, 0.045f, 0.009f)));
    g_next.set(Part(bt, T().button, Vector3(kSideX, 0, 0), Vector3(kSideW, kSideH, 0.009f)));

    if (Il2CppObject *canvas = Canvas(bt, Vector3(0, 0, -0.0105f), kBoardW + 2 * kSideX, kBoardH + 0.1f)) {
        Il2CppObject *ct = Tr(canvas);
        Label(ct, "t3wtr4fvr", Vector3(0, kTitleY, 0), kBtnW, 0.045f, 32, &g_title);
        for (int r = 0; r < kRows; r++)
            g_rowLabel[r].set(Label(ct, "", Vector3(0, RowY(r), 0), kBtnW, kBtnH, 19, &g_rowText[r]));
        Label(ct, "<", Vector3(-kSideX, 0, -0.0005f), kSideW, 0.05f, 34, nullptr);
        Label(ct, ">", Vector3(kSideX, 0, -0.0005f), kSideW, 0.05f, 34, nullptr);
        Label(ct, "", Vector3(0, -kBoardH / 2 - 0.02f, 0), kBoardW + 0.1f, 0.03f, 15, &g_tipText);
        char ver[64];
        snprintf(ver, sizeof ver, "t3wtr4fvr build %s", ym::api()->gameVersion);
        Label(ct, ver, Vector3(0, -kBoardH / 2 + 0.012f, 0), kBoardW, 0.02f, 11, nullptr);
    }
    // Pointer: a small ball on the index finger of the other hand
    if (Il2CppObject *p = Prim(0, T().bgB, false)) {
        G.trSetLocalScale[Tr(p)](Vector3(0.01f, 0.01f, 0.01f));
        g_pointer.set(p);
        PointerToHand();
    }
    // Notifications: a line of text low in your view, like ii's notifications
    if (Il2CppObject *n = Canvas(g_head.get(), Vector3(0, -0.12f, 0.6f), 0.5f, 0.05f)) {
        g_notifyGo.set(n);
        Label(Tr(n), "", Vector3(0, 0, 0), 0.5f, 0.05f, 22, &g_notifyText);
        G.goSetActive[n](false);
    }
    G.goSetActive[menu](false);
    Show(g_pointer, false);
    g_built = true;
    g_dirty = true;
}

static void Notify(const char *fmt, ...) {
    if (!f_notify || !Alive(g_notifyGo.get())) return;
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    SetText(g_notifyText, buf);
    G.goSetActive[g_notifyGo.get()](true);
    g_notifyUntil = g_time + 2.0;
}

// Relabels and recolors the rows. Only runs after something changed.
static void RenderPage() {
    BuildView();
    for (int r = 0; r < kRows; r++) {
        int i = g_page * kRows + r;
        bool used = i < (int)g_view.size();
        Show(g_rowCube[r], used);
        Show(g_rowLabel[r], used);
        if (!used) continue;
        Item *it = g_view[i];
        bool on = it->kind == TOGGLE && *it->flag;
        SetText(g_rowText[r], it->label);
        SetColor(g_rowCube[r].get(), r == g_flashRow ? Color(1, 0.9f, 0.6f, 1) : on ? T().buttonOn : T().button);
        if (Alive(g_rowText[r].get())) G.graphicSetColor[g_rowText[r].get()](on ? Color(1, 1, 1, 1) : T().title);
    }
    bool pages = PageCount() > 1;
    Show(g_prev, pages);
    Show(g_next, pages);
    SetColor(g_prev.get(), T().button);
    SetColor(g_next.get(), T().button);
    SetPanelColor(g_outline, g_outEx, T().title);
    UpdateDecor();
    SetColor(g_titleBar.get(), T().buttonOn);
    SetColor(g_pointer.get(), T().bgB);
    if (Alive(g_title.get())) G.graphicSetColor[g_title.get()](Color(1, 1, 1, 1));
    g_nextTitle = 0;                     // refresh the title now
    g_dirty = false;
}

static int Hex(float v) { return (int)(v < 0 ? 0 : v > 1 ? 255 : v * 255); }

// The title is "t3wtr4fvr" with a moving red-orange-yellow gradient, one color per letter
// (rich text). Rebuilt ten times a second, not every frame.
static void UpdateTitle() {
    if (g_time < g_nextTitle) return;
    g_nextTitle = g_time + 0.1;
    static const Color kFire[] = {Color(1, 0.1f, 0.05f, 1), Color(1, 0.45f, 0, 1), Color(1, 0.8f, 0.2f, 1), Color(1, 0.45f, 0, 1)};
    char buf[512];
    int n = 0;
    const char *word = "t3wtr4fvr";
    for (int i = 0; word[i]; i++) {
        float t = (float)g_time * 1.5f - i * 0.35f;
        Color c;
        if (f_rainbow) c = Rainbow(t * 0.2f);
        else {
            float f = t - floorf(t / 4) * 4;          // 0..4 along the fire colors
            int k = (int)f;
            c = Lerp(kFire[k], kFire[(k + 1) % 4], f - k);
        }
        n += snprintf(buf + n, sizeof buf - n, "<b><color=#%02X%02X%02X>%c</color></b>", Hex(c.r), Hex(c.g), Hex(c.b), word[i]);
    }
    n += snprintf(buf + n, sizeof buf - n, " <color=grey>[</color><color=white>%d</color><color=grey>]</color>", g_page + 1);
    if (f_fps) n += snprintf(buf + n, sizeof buf - n, "<size=16>  %d fps</size>", (int)(g_fps + 0.5f));
    if (f_clock) {
        time_t now = time(nullptr);
        struct tm lt;
        localtime_r(&now, &lt);
        if (f_clock24) n += snprintf(buf + n, sizeof buf - n, "<size=16>  %d:%02d</size>", lt.tm_hour, lt.tm_min);
        else n += snprintf(buf + n, sizeof buf - n, "<size=16>  %d:%02d</size>", lt.tm_hour % 12 ? lt.tm_hour % 12 : 12, lt.tm_min);
    }
    if (f_speedText) {
        Vector3 v;
        if (GetVel(v)) n += snprintf(buf + n, sizeof buf - n, "<size=16>  %.1f m/s</size>", sqrtf(v.x * v.x + v.z * v.z));
    }
    if (f_heightText) {
        Vector3 b;
        if (BodyPos(b)) n += snprintf(buf + n, sizeof buf - n, "<size=16>  %.1f m</size>", b.y);
    }
    SetText(g_title, buf);
}

static void ShowMenu(bool show) {
    if (show == g_menuShown) return;
    g_menuShown = show;
    if (show) { g_openTime = g_time; g_dirty = true; g_hover = -2; SetText(g_tipText, ""); }
    Show(g_menu, show);
    Show(g_pointer, show);
    g_touching = -2;
}

static float Dot(Vector3 a, Vector3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Like ii's menu: the menu follows your hand and is tipped 45 degrees, so you read it
// by turning your palm toward your face. "Floating Menu" instead keeps the board above
// your hand, always facing you.
static void PlaceMenu() {
    Il2CppObject *menu = g_menu.get(), *board = g_board.get(), *head = g_head.get();
    Il2CppObject *hand = f_rightHand ? g_rightHand.get() : g_leftHand.get();
    if (!Alive(menu) || !Alive(board) || !hand || !head) return;
    Il2CppObject *t = Tr(menu), *bt = Tr(board);
    Vector3 handPos = G.trGetPosition[hand]();
    if (!G.trGetRotation.IsValid()) {                  // fallback: float above the hand, facing you
        Vector3 center = handPos + Vector3(0, 0.25f, 0), away = center - G.trGetPosition[head]();
        if (away.x * away.x + away.y * away.y + away.z * away.z < 1e-6f) return;
        G.trSetPosition[t](handPos);
        G.trSetPosition[bt](center);
        G.trSetRotation[bt](Quaternion::LookRotation(away));
        return;
    }
    const float s = 0.38268343f, c = 0.92387953f;                  // sin/cos of 22.5 degrees
    Quaternion rot = G.trGetRotation[hand]() * Quaternion(-s, 0, 0, c);   // tip -45 degrees on x
    if (f_rightHand) rot = rot * Quaternion(0, 0, 1, 0);           // mirrored: 180 degrees on z
    G.trSetPosition[t](handPos);
    G.trSetRotation[t](rot);
    G.trSetLocalPosition[bt](Vector3(0.1f, 0, 0));
    G.trSetLocalRotation[bt](Quaternion::LookRotation(Vector3(-1, 0, 0), Vector3(0, 0, 1)));
}

static void Press(Item *it) {
    switch (it->kind) {
    case OPEN:
        g_cat = it->target;
        g_page = 0;
        break;
    case TOGGLE:
        *it->flag = !*it->flag;
        if (it->fn) it->fn();
        Notify("[%s] %s", *it->flag ? "ON" : "OFF", it->label);
        break;
    case ACTION:
        if (it->fn) it->fn();
        break;
    }
    g_dirty = true;
    ym::log("starter-menu: %s", it->label);
}

// Which part of the menu the pointer touches: row 0..7, -1 = <, kRows = >, -2 = nothing.
static int Touching() {
    Il2CppObject *board = g_board.get(), *pointer = g_pointer.get();
    if (!Alive(board) || !Alive(pointer)) return -2;
    Vector3 p = G.trInverseTransformPoint[Tr(board)](G.trGetPosition[Tr(pointer)]());
    if (p.z < -0.035f || p.z > 0.015f) return -2;
    if (fabsf(fabsf(p.x) - kSideX) < kSideW / 2 && fabsf(p.y) < kSideH / 2 && PageCount() > 1)
        return p.x < 0 ? -1 : kRows;
    if (fabsf(p.x) >= kBtnW / 2) return -2;
    for (int r = 0; r < kRows; r++)
        if (fabsf(p.y - RowY(r)) < kBtnH / 2) return r;
    return -2;
}

static void Buzz() {
    if (!f_haptics || !G.haptic.IsValid()) return;
    XRDevice d = G.deviceAtNode(f_rightHand ? 4 : 5);    // the hand that presses
    if (d.initialized) G.haptic(d.id, 0u, 0.35f, 0.04f);
}

static void CheckPress() {
    int hit = Touching();
    if (hit != g_hover) {                                 // tooltip for the button under your finger
        g_hover = hit;
        int i = g_page * kRows + hit;
        if (hit >= 0 && hit < kRows && i < (int)g_view.size()) SetText(g_tipText, TipFor(g_view[i]->label));
    }
    if (hit != -2 && hit != g_touching && g_time >= g_nextPress) {   // one press per touch
        g_nextPress = g_time + 0.25;
        Buzz();
        if (hit >= 0 && hit < kRows) { g_flashRow = hit; g_flashUntil = g_time + 0.12; }
        if (hit == -1 || hit == kRows) {
            int n = PageCount();
            g_page = (g_page + (hit == kRows ? 1 : n - 1)) % n;
            g_dirty = true;
        } else {
            int i = g_page * kRows + hit;
            if (i < (int)g_view.size()) Press(g_view[i]);
        }
    }
    g_touching = hit;
}

static void AnimateMenu() {
    float open = (float)(g_time - g_openTime) / 0.15f;     // pop open like ii's menu
    if (open <= 1.05f && Alive(g_menu.get())) {
        float k = open >= 1 ? 1 : 1 - (1 - open) * (1 - open);
        G.trSetLocalScale[Tr(g_menu.get())](Vector3(k * g_menuSize, k * g_menuSize, k * g_menuSize));
    }
    if (g_flashRow != -2 && g_time >= g_flashUntil) { g_flashRow = -2; g_dirty = true; }
    static double nextTheme = 0;
    if (f_autoTheme && g_time >= nextTheme) { nextTheme = g_time + 4.0; g_theme = (g_theme + 1) % kThemeCount; g_dirty = true; }
    float t = 0.5f + 0.5f * sinf((float)g_time * 2.0f);
    Color c = f_rainbow ? Rainbow((float)g_time * 0.15f) : Lerp(T().bgA, T().bgB, t);
    SetPanelColor(g_bg, g_bgEx, c);
}

// ---- features (local only) -----------------------------------------------------

static Il2CppObject *PlayerTransform() {
    Il2CppObject *player = g_player.get();
    return player ? G.compTransform[player]() : nullptr;
}

// Your body's velocity, through the game's own player functions (Rigidbody as backup).
static bool GetVel(Vector3 &v) {
    Il2CppObject *player = g_player.get(), *rb = g_rigidbody.get();
    if (player && G.plGetVelocity.IsValid()) { v = G.plGetVelocity[player](); return true; }
    if (rb && G.rbGetVelocity.IsValid()) { v = G.rbGetVelocity[rb](); return true; }
    return false;
}
static void SetVel(Vector3 v) {
    Il2CppObject *player = g_player.get(), *rb = g_rigidbody.get();
    if (player && G.plSetVelocity.IsValid()) G.plSetVelocity[player](v);
    else if (rb && G.rbSetVelocity.IsValid()) G.rbSetVelocity[rb](v);
}

// Where your body is (the body collider), which is what teleports aim for.
static bool BodyPos(Vector3 &out) {
    Il2CppObject *player = g_player.get(), *t = PlayerTransform();
    if (!player || !t) return false;
    Il2CppObject *body = G.bodyCollider.IsValid() ? G.bodyCollider[player]() : nullptr;
    out = body ? G.trGetPosition[G.compTransform[body]()]() : G.trGetPosition[t]();
    return true;
}
static void StopMoving() { SetVel(Vector3(0, 0, 0)); }
// Moves your body to pos, using the game's own TeleportTo when it has one.
static void TeleportPlayer(Vector3 pos) {
    Il2CppObject *player = g_player.get(), *t = PlayerTransform();
    Vector3 body;
    if (!player || !t || !BodyPos(body)) return;
    Vector3 target = pos - body + G.trGetPosition[t]();
    if (G.teleportTo.IsValid()) G.teleportTo[player](target);
    else G.trSetPosition[t](target);
    StopMoving();
}

static void SaveSpot() {
    if (BodyPos(g_spot)) { g_haveSpot = true; Notify("Spot saved"); }
}
static void GoToSpot() {
    if (!g_haveSpot) { Notify("Save a spot first"); return; }
    TeleportPlayer(g_spot);
    Notify("Teleported to your spot");
}

static void SaveSpot2() {
    if (BodyPos(g_spot2)) { g_haveSpot2 = true; Notify("Spot 2 saved"); }
}
static void GoToSpot2() {
    if (!g_haveSpot2) { Notify("Save spot 2 first"); return; }
    TeleportPlayer(g_spot2);
    Notify("Teleported to spot 2");
}

static const float kIronSpeeds[] = {15, 12, 8, 4};
static int g_ironSpeed = 0;
static void NextIronSpeed() { g_ironSpeed = (g_ironSpeed + 1) % 4; Notify("Iron Man speed: %d", (int)kIronSpeeds[g_ironSpeed]); }
static void SetMenuSize(float sz) {
    g_menuSize = sz;
    if (Alive(g_menu.get())) G.trSetLocalScale[Tr(g_menu.get())](Vector3(sz, sz, sz));
    Notify("Menu size: %d%%", (int)(sz * 100 + 0.5f));
}
static void NextMenuSize() {
    static const float kSizes[] = {1.0f, 1.25f, 1.5f, 0.75f};
    static const char *kNames[] = {"normal", "big", "huge", "small"};
    static int i = 0;
    i = (i + 1) % 4;
    g_menuSize = kSizes[i];
    if (Alive(g_menu.get())) G.trSetLocalScale[Tr(g_menu.get())](Vector3(g_menuSize, g_menuSize, g_menuSize));
    Notify("Menu size: %s", kNames[i]);
}
static void LaunchUp() {
    Vector3 v;
    if (GetVel(v)) { v.y = 14.0f; SetVel(v); Notify("Launch!"); }
}
static void StopAction() { StopMoving(); Notify("Stopped"); }
static void NextTheme() { g_theme = (g_theme + 1) % kThemeCount; Notify("Theme: %s", T().name); }
static void AllOff() {
    for (auto &it : g_items) if (it.kind == TOGGLE && it.flag != &f_notify) *it.flag = false;   // ticks undo each mod
    g_dance = -1;
    g_dupeArmed = 0;
    g_itemQueue.clear();
    PointerToHand();
    Notify("Everything off");
}

static void FlyTick(const Buttons &in, float dt) {
    Il2CppObject *head = g_head.get(), *t = PlayerTransform();
    if (!f_fly || !in.a || !head || !t) return;
    Vector3 pos = G.trGetPosition[t]() + G.trGetForward[head]() * ((g_flyLevel > 0 ? g_flyLevel * 1.5f : (f_fastFly ? 20.0f : 8.0f)) * dt);
    G.trSetPosition[t](pos);
    StopMoving();
}

static void LowGravityTick(const Buttons &in, float dt) {
    Vector3 v;
    if (!f_lowGravity || f_noGravity || (f_fly && in.a) || !GetVel(v)) return;
    SetVel(v + Vector3(0, 9.81f * g_gravCancel * dt, 0));   // cancels part of gravity (70% unless you picked a level)
}

static void PlatformFor(bool held, Ref &platform, Ref &hand) {
    if (!held || !f_platforms) {
        Kill(platform);
        return;
    }
    if (platform.get()) return;                             // already standing on one
    if (!hand.get()) return;
    Il2CppObject *p = Cube(T().button, true);               // keeps its collider: you stand on it
    if (!p) return;
    // Hands only collide with the layers in the player's locomotionEnabledLayers (like
    // Gorilla Tag), so the platform goes on the first of those layers.
    static int layer = -2;
    if (layer == -2) {
        layer = -1;
        int mask = G.locomotionLayers.IsValid() && g_player.get() ? G.locomotionLayers[g_player.get()]() : 0;
        for (int i = 0; i < 32 && layer < 0; i++) if (mask & (1 << i)) layer = i;
        ym::log("starter-menu: hand layers 0x%x, platforms on layer %d", mask, layer);
    }
    if (layer >= 0 && G.goSetLayer.IsValid()) G.goSetLayer[p](layer);
    Il2CppObject *t = Tr(p);
    G.trSetPosition[t](G.trGetPosition[hand.get()]() - Vector3(0, 0.08f, 0));
    G.trSetLocalScale[t](Vector3(0.28f, 0.02f, 0.28f));
    platform.set(p);
}

static void VelocityFlyTick(const Buttons &in) {
    Il2CppObject *hand = g_rightHand.get();
    if (!f_velocityFly || !in.a || !hand) return;
    SetVel(G.trGetForward[hand]() * 20.0f);
}

static Vector3 Flat(Vector3 v);

// Car: like Fly, but along the ground. Holding A drives you the way you look, flat, and
// you hover a little above the ground (a raycast down finds it) so the bumps on blocks
// don't catch you. Off a ledge, with no ground close below, you fall normally.
static const float kCarSpeeds[] = {4.0f, 6.0f, 9.0f};
static const char *kCarNames[] = {"slow", "normal", "fast"};
static int g_carSpeed = 1;
static float g_carHeight = -1;               // your head's height above the ground, measured once
static void NextCarSpeed() { g_carSpeed = (g_carSpeed + 1) % 3; Notify("Car: %s", kCarNames[g_carSpeed]); }
static void CarTick(const Buttons &in) {
    Il2CppObject *head = g_head.get(), *player = g_player.get();
    Vector3 v;
    if (!f_car) { g_carHeight = -1; return; }
    if (!in.a || !head || !player || !GetVel(v) || !G.raycast.IsValid()) return;
    int mask = G.locomotionLayers.IsValid() ? G.locomotionLayers[player]() : -5;
    Vector3 hp = G.trGetPosition[head]();
    BNM::Structures::Unity::RaycastHit hit{};
    bool ground = G.raycast(hp, Vector3(0, -1, 0), &hit, 3.0f, mask ? mask : -5);
    Vector3 f = Flat(G.trGetForward[head]()) * kCarSpeeds[g_carSpeed];
    float vy = fminf(v.y, 1.0f);
    if (ground) {
        if (g_carHeight < 0) g_carHeight = hp.y - hit.point.y;      // your standing height
        float target = hit.point.y + g_carHeight + 0.15f;            // float 15 cm higher
        vy = (target - hp.y) * 8.0f;
        if (G.plNoGravityFor.IsValid()) G.plNoGravityFor[player](0.1f);
    }
    SetVel(Vector3(f.x, vy, f.z));
}

static void IronManTick(const Buttons &in, float dt) {
    Vector3 v;
    if (!f_ironMan || !G.trGetRight.IsValid() || (!in.x && !in.a) || !GetVel(v)) return;
    float speed = kIronSpeeds[g_ironSpeed] * dt;
    if (in.x && g_leftHand.get()) v = v + G.trGetRight[g_leftHand.get()]() * -speed;
    if (in.a && g_rightHand.get()) v = v + G.trGetRight[g_rightHand.get()]() * speed;
    SetVel(v);
}

// Speed Boost and No Gravity change your own player; turning them off puts the old
// values back. Checked every frame so they also come back after a room change.
// More local movement mods. They only change your own velocity, like Fly and Low Gravity.
static float g_lastVy = 0;
static bool g_prevX = false, g_prevY = false;
static double g_nextAirJump = 0, g_nextDash = 0;
static void TeleportForward() {
    Il2CppObject *head = g_head.get();
    Vector3 b;
    if (!head || !BodyPos(b)) return;
    TeleportPlayer(b + Flat(G.trGetForward[head]()) * 5.0f);
    Notify("Teleported 5 m forward");
}
static void TeleportUp() {
    Vector3 b;
    if (!BodyPos(b)) return;
    TeleportPlayer(b + Vector3(0, 5.0f, 0));
    Notify("Teleported 5 m up");
}
static void MoreMovementTick(const Buttons &in, float dt) {
    bool any = f_slowFall || f_airJump || f_dash || f_hover || f_bouncy || f_jetpack;
    bool x = in.x, y = in.y;
    bool xEdge = x && !g_prevX, yEdge = y && !g_prevY;
    g_prevX = x; g_prevY = y;
    Vector3 v;
    if (!any || !GetVel(v)) { g_lastVy = 0; return; }
    Il2CppObject *head = g_head.get(), *player = g_player.get();
    if (f_hover && in.ltrig && g_dance < 0) {          // hang in the air
        SetVel(Vector3(0, 0, 0));
        if (player && G.plNoGravityFor.IsValid()) G.plNoGravityFor[player](0.25f);
        g_lastVy = 0;
        return;
    }
    if (f_airJump && yEdge && g_time >= g_nextAirJump) { g_nextAirJump = g_time + 0.35; v.y = 6.5f; SetVel(v); }
    if (f_dash && xEdge && head && !(f_ironMan && in.a) && g_time >= g_nextDash) {
        g_nextDash = g_time + 0.8;
        Vector3 f = Flat(G.trGetForward[head]());
        v.x += f.x * g_dashPower; v.z += f.z * g_dashPower;
        SetVel(v);
    }
    if (f_jetpack && in.b && !f_rightHand) { v.y = fminf(v.y + 18.0f * dt, 7.0f); SetVel(v); }   // thrust up while B is held
    if (f_slowFall && !f_noGravity && v.y < -2.5f) { v.y = -2.5f; SetVel(v); }       // glide down gently
    if (f_bouncy && g_lastVy < -4.0f && v.y > g_lastVy + 3.0f) { v.y = -g_lastVy * 0.8f; SetVel(v); }   // landed hard
    g_lastVy = v.y;
}
static float g_oldJumpSpeed = 0, g_oldJumpMultiplier = 0;
static Il2CppObject *g_boosted = nullptr;      // which player has the boost (compared only)
static bool g_gravityOff = false;

static void ApplySpeedBoost() {
    // Speed Boost and Super Jump both change your jump numbers. Your own values are saved the
    // first time either turns on and put back when both are off.
    Il2CppObject *player = g_player.get();
    bool ok = G.maxJumpSpeed.IsValid() && G.jumpMultiplier.IsValid();
    bool want = f_speedBoost || f_superJump || g_jumpLevel > 0;
    if (want && ok && player) {
        if (g_boosted != player) {
            g_oldJumpSpeed = G.maxJumpSpeed[player]();
            g_oldJumpMultiplier = G.jumpMultiplier[player]();
            g_boosted = player;
        }
        float spd = f_superJump ? 14.0f : 9.0f, mul = f_superJump ? 3.0f : 1.5f;
        if (g_jumpLevel > 0) { spd = 6.0f + g_jumpLevel * 0.8f; mul = 1.0f + g_jumpLevel * 0.2f; }
        G.maxJumpSpeed[player] = spd;
        G.jumpMultiplier[player] = mul;
    } else if (!want && g_boosted) {
        if (ok && player == g_boosted) {
            G.maxJumpSpeed[player] = g_oldJumpSpeed;
            G.jumpMultiplier[player] = g_oldJumpMultiplier;
        }
        g_boosted = nullptr;
    }
}

static void ApplyGravity() {
    // The game's own "no gravity for a while" (what its levitation items use), renewed
    // every frame while the mod is on; it wears off by itself once you turn it off.
    Il2CppObject *player = g_player.get();
    if (f_noGravity && player && G.plNoGravityFor.IsValid()) G.plNoGravityFor[player](0.25f);
    g_gravityOff = f_noGravity;
}

// Noclip: while you hold the right trigger you pass through everything. Yeeps moves
// your body with its own layer lists (locomotionEnabledLayers for your hands,
// collisionEnabledLayers for your body), so those are emptied, physics ignores your
// body against those layers, and your own colliders are off. Gravity stays normal, like
// Gorilla Tag noclip. All of it goes back when you let go. Only your own player changes; nothing is sent.
static bool g_noclipOn = false;
static Ref g_noclipOff[24];                    // your own colliders switched off during noclip
static int g_noclipOffN = 0;
// Every solid collider on your own player (body, head, hands...), switched off while
// noclip is held so nothing of you hits the level.
static void PlayerColliders(bool off) {
    if (!off) {
        for (int i = 0; i < g_noclipOffN; i++) {
            if (Alive(g_noclipOff[i].get())) G.colliderSetEnabled[g_noclipOff[i].get()](true);
            g_noclipOff[i].clear();
        }
        g_noclipOffN = 0;
        return;
    }
    Il2CppObject *player = g_player.get();
    if (!player || !G.colliderSetEnabled.IsValid()) return;
    auto offOne = [](Il2CppObject *c) {
        if (!Alive(c) || g_noclipOffN >= 24 || !G.behaviourGetEnabledCollider[c]()) return;
        if (G.colliderIsTrigger.IsValid() && G.colliderIsTrigger[c]()) return;
        for (int i = 0; i < g_noclipOffN; i++) if (g_noclipOff[i].get() == c) return;
        G.colliderSetEnabled[c](false);
        g_noclipOff[g_noclipOffN++].set(c);
    };
    // Your body and head colliders (fields on the player), then every solid collider in
    // the rig they belong to.
    Il2CppObject *own[2] = {G.bodyCollider.IsValid() ? G.bodyCollider[player]() : nullptr,
                            G.headCollider.IsValid() ? G.headCollider[player]() : nullptr};
    for (Il2CppObject *c : own) {
        if (!c) continue;
        offOne(c);
        Il2CppObject *rig = G.compGameObject[G.trGetRoot[G.compTransform[c]()]()]();
        auto *arr = rig && G.getComponentsInChildren.IsValid()
                        ? (uint8_t *)G.getComponentsInChildren[rig]((Il2CppObject *)G.tCollider, false) : nullptr;
        size_t n = arr ? *(uintptr_t *)(arr + 0x18) : 0;
        for (size_t i = 0; i < n; i++) offOne(((Il2CppObject **)(arr + 0x20))[i]);
    }
}
static int g_savedLoco = 0, g_savedColl = 0, g_bodyLayer = -1;
static void NoclipTick(const Buttons &in) {
    Il2CppObject *player = g_player.get();
    bool want = f_noclip && in.rtrig && player;
    if (want == g_noclipOn) return;
    g_noclipOn = want;
    if (!player || !G.locomotionLayers.IsValid()) return;
    if (want) {
        g_savedLoco = G.locomotionLayers[player]();
        g_savedColl = G.collisionLayers.IsValid() ? G.collisionLayers[player]() : 0;
        Il2CppObject *body = G.bodyCollider.IsValid() ? G.bodyCollider[player]() : nullptr;
        Il2CppObject *bodyGo = body ? G.compGameObject[body]() : nullptr;
        g_bodyLayer = bodyGo && G.goGetLayer.IsValid() ? G.goGetLayer[bodyGo]() : -1;
    }
    int mask = g_savedLoco | g_savedColl;
    G.locomotionLayers[player] = want ? 0 : g_savedLoco;
    if (G.collisionLayers.IsValid()) G.collisionLayers[player] = want ? 0 : g_savedColl;
    if (g_bodyLayer >= 0 && G.ignoreLayers.IsValid())
        for (int l = 0; l < 32; l++)
            if (mask & (1 << l)) G.ignoreLayers(g_bodyLayer, l, want);
    PlayerColliders(want);
    if (want) ym::log("starter-menu: noclip on (layers 0x%x, body layer %d, %d colliders off)", mask, g_bodyLayer, g_noclipOffN);
}

// Teleport Gun: hold right grip to aim (a line and a ball show where you land), press
// the right trigger to go there.
static Ref g_gunLine, g_gunLineGo, g_gunBall;
static bool g_gunTrigger = false;
static void TeleportGunTick(const Buttons &in) {
    bool aiming = (f_tpGun || f_itemGun) && in.rgrip;
    if (!aiming) {
        Show(g_gunLineGo, false);
        Show(g_gunBall, false);
        g_gunTrigger = in.rtrig;
        return;
    }
    Il2CppObject *hand = g_rightHand.get();
    if (!hand || !G.raycast.IsValid() || !G.lrSetPosition.IsValid()) return;
    if (!g_gunBall.get()) {                              // made once per room
        Il2CppObject *ball = Prim(0, T().bgB, false);
        Il2CppObject *lineGo = Class("UnityEngine", "GameObject").CreateNewObjectParameters(CreateMonoString("T3wtr4fvrGunLine"));
        Il2CppObject *line = lineGo && G.tLine ? G.goAddComponent[lineGo](G.tLine) : nullptr;
        if (!ball || !line) return;
        G.trSetLocalScale[Tr(ball)](Vector3(0.1f, 0.1f, 0.1f));
        if (Il2CppObject *m = G.rendererGetMaterial[line]()) {
            if (G.shader.get()) G.matSetShader[m](G.shader.get());
            G.matSetColor[m](T().bgB);
        }
        G.lrSetStartWidth[line](0.025f);
        G.lrSetEndWidth[line](0.025f);
        G.lrSetPositionCount[line](2);
        G.lrSetUseWorldSpace[line](true);
        g_gunBall.set(ball);
        g_gunLine.set(line);
        g_gunLineGo.set(lineGo);
    }
    Vector3 start = G.trGetPosition[hand](), dir = G.trGetForward[hand]();
    BNM::Structures::Unity::RaycastHit hit{};
    bool found = G.raycast(start + dir * 0.25f, dir, &hit, 512.0f, -5);   // Physics.DefaultRaycastLayers
    Vector3 end = found ? hit.point : start + dir * 512.0f;
    Show(g_gunLineGo, true);
    Show(g_gunBall, true);
    G.trSetPosition[Tr(g_gunBall.get())](end);
    G.lrSetPosition[g_gunLine.get()](0, start);
    G.lrSetPosition[g_gunLine.get()](1, end);
    SetColor(g_gunBall.get(), in.rtrig ? T().buttonOn : T().bgB);
    if (in.rtrig && !g_gunTrigger && found) {
        if (f_itemGun) {                                  // Spawn Item Gun: an item at that spot
            if (g_time >= g_nextItem && LeftHandItem()) {
                g_nextItem = g_time + 0.5;
                g_itemQueue.push_back({end + Vector3(0, 0.3f, 0), Vector3(0, 0, 0)});
            }
        } else TeleportPlayer(end + Vector3(0, 1.0f, 0));
    }
    g_gunTrigger = in.rtrig;
}

// ---- vector helpers ----------------------------------------------------------------

static Vector3 Cross(Vector3 a, Vector3 b) { return Vector3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static Vector3 Flat(Vector3 v) {                      // v without its up/down part, length 1
    v.y = 0;
    float l = sqrtf(v.x * v.x + v.z * v.z);
    return l > 1e-4f ? v * (1.0f / l) : Vector3(0, 0, 1);
}

// ---- rig mods: dances -----------------------------------------------------------
// While you hold the left trigger, your hands are moved through a dance instead of
// following your controllers. The game sends your hand positions to the room as
// usual, so other players see you dance. Nothing extra is sent.

// The dance buttons work like radio buttons: picking one turns the others off,
// pressing the picked one again turns dancing off.
static float g_funnySpeed = 9.0f;
static void NextFunnySpeed() {
    static int i = 1;
    static const float kS[] = {6.0f, 9.0f, 13.0f};
    static const char *kN[] = {"slow", "normal", "fast"};
    i = (i + 1) % 3;
    g_funnySpeed = kS[i];
    Notify("Funny Run: %s", kN[i]);
}
static void DancePicked() {
    int picked = -1;
    for (int i = 0; i < kDances; i++) if (g_danceSel[i] && i != g_dance) picked = i;
    for (int i = 0; i < kDances; i++) g_danceSel[i] = (i == picked);
    g_dance = picked;
    if (picked >= 0) Notify("Hold left trigger to dance");
}
static float Rand01() { static uint32_t x = 2463534242u; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return (x & 0xffff) / 65535.0f; }

// The controller trackers would put your hands back every frame, so they are paused
// while you dance (on the hand and the two objects above it) and turned back on after.
static Ref g_paused[12];
static int g_pausedN = 0;
static bool g_dancing = false;
static void PauseTrackers(bool pause) {
    if (!pause) {
        for (int i = 0; i < g_pausedN; i++) {
            if (Alive(g_paused[i].get())) G.behaviourSetEnabled[g_paused[i].get()](true);
            g_paused[i].clear();
        }
        g_pausedN = 0;
        return;
    }
    if (!G.behaviourSetEnabled.IsValid() || !G.trGetParent.IsValid()) return;
    for (Il2CppObject *t : {g_leftHand.get(), g_rightHand.get()})
        for (int up = 0; up < 3 && t; up++, t = G.trGetParent[t]())
            for (BNM::MonoType *type : G.trackerTypes) {
                Il2CppObject *go = G.compGameObject[t]();
                Il2CppObject *c = go ? G.goGetComponent[go]((Il2CppObject *)type) : nullptr;
                if (!c || !G.behaviourGetEnabled[c]() || g_pausedN >= 12) continue;
                G.behaviourSetEnabled[c](false);
                g_paused[g_pausedN++].set(c);
            }
}

// 96 generated poses: 8 kinds of motion x 3 speeds x 2 sizes x (both hands together or taking turns).
static void GenPose(int idx, float t, Vector3 shoulder, Vector3 r, Vector3 f, Vector3 up, Vector3 &R, Vector3 &L) {
    static const float kSpeed[3] = {3.0f, 6.0f, 10.0f};
    int fam = idx / 12, v = idx % 12;
    float size = ((v / 2) % 2) ? 0.4f : 0.2f;
    float p = t * kSpeed[v / 4], q = (v % 2) ? p + 3.14159265f : p;
    switch (fam) {
    case 0:   // Circles
        R = shoulder + f * 0.4f + r * (0.15f + size * cosf(p)) + up * (size * sinf(p));
        L = shoulder + f * 0.4f - r * (0.15f + size * cosf(q)) + up * (size * sinf(q));
        break;
    case 1:   // Figure 8
        R = shoulder + f * 0.4f + r * (0.15f + size * sinf(p)) + up * (size * 0.5f * sinf(2 * p));
        L = shoulder + f * 0.4f - r * (0.15f + size * sinf(q)) + up * (size * 0.5f * sinf(2 * q));
        break;
    case 2:   // Sweeps
        R = shoulder + f * 0.4f + r * (size * 1.5f * sinf(p));
        L = shoulder + f * 0.4f + r * (size * 1.5f * sinf(q));
        break;
    case 3:   // Pumps
        R = shoulder + f * (0.3f + size * sinf(p)) + r * 0.2f;
        L = shoulder + f * (0.3f + size * sinf(q)) - r * 0.2f;
        break;
    case 4:   // Bobs
        R = shoulder + r * 0.3f + f * 0.2f + up * (-0.2f + size * sinf(p));
        L = shoulder - r * 0.3f + f * 0.2f + up * (-0.2f + size * sinf(q));
        break;
    case 5:   // Flaps
        R = shoulder + r * 0.55f + up * (size * 1.5f * sinf(p));
        L = shoulder - r * 0.55f + up * (size * 1.5f * sinf(q));
        break;
    case 6:   // Spins
        R = shoulder + r * ((0.3f + size) * cosf(p)) + f * ((0.3f + size) * sinf(p)) + up * 0.05f;
        L = shoulder - r * ((0.3f + size) * cosf(q)) - f * ((0.3f + size) * sinf(q)) + up * 0.05f;
        break;
    default:  // Waves
        R = shoulder + r * (0.3f + size * sinf(p)) + up * 0.45f + f * 0.1f;
        L = shoulder - r * (0.3f + size * sinf(q)) + up * 0.45f + f * 0.1f;
        break;
    }
}

static void DanceTick(const Buttons &in, float dt) {
    Il2CppObject *head = g_head.get(), *lh = g_leftHand.get(), *rh = g_rightHand.get();
    bool want = g_dance >= 0 && in.ltrig && head && lh && rh;
    if (want != g_dancing) {
        PauseTrackers(want);
        g_dancing = want;
        if (want) ym::log("starter-menu: dancing, paused %d trackers", g_pausedN);
    }
    if (!want) return;
    Vector3 hp = G.trGetPosition[head](), f = Flat(G.trGetForward[head]()), up(0, 1, 0);
    Vector3 r = Cross(up, f);                            // your right
    Vector3 shoulder = hp + up * -0.25f;
    float t = (float)g_time * g_danceSpeed, s = sinf(t * 6), c = cosf(t * 6);
    Vector3 L, R;                                        // hand positions
    switch (g_dance) {
    case 0:   // Wave: right hand up and waving, left hand on the hip
        R = shoulder + r * (0.35f + 0.12f * s) + up * 0.45f + f * 0.1f;
        L = shoulder + r * -0.25f + up * -0.25f + f * 0.05f;
        break;
    case 1:   // Helicopter: both hands spin around you
        R = shoulder + r * (0.6f * cosf(t * 8)) + f * (0.6f * sinf(t * 8)) + up * 0.15f;
        L = shoulder - r * (0.6f * cosf(t * 8)) - f * (0.6f * sinf(t * 8)) + up * 0.15f;
        break;
    case 2:   // T-Pose
        R = shoulder + r * 0.7f + up * 0.1f;
        L = shoulder - r * 0.7f + up * 0.1f;
        break;
    case 3:   // Windmill: big circles at your sides
        R = shoulder + r * 0.4f + up * (0.5f * sinf(-t * 5)) + f * (0.5f * cosf(-t * 5));
        L = shoulder - r * 0.4f - up * (0.5f * sinf(-t * 5)) - f * (0.5f * cosf(-t * 5));
        break;
    case 4: {  // Swim: breaststroke
        float p = t * 3;
        R = shoulder + f * (0.3f + 0.25f * cosf(p)) + r * (0.08f + 0.35f * fabsf(sinf(p)));
        L = shoulder + f * (0.3f + 0.25f * cosf(p)) - r * (0.08f + 0.35f * fabsf(sinf(p)));
        break;
    }
    case 5: {  // Jumping jacks: arms from down by your sides to up over your head
        float a = (sinf(t * 6) * 0.5f + 0.5f) * 3.1f - 1.55f;
        R = shoulder + r * (0.6f * cosf(a)) + up * (0.6f * sinf(a));
        L = shoulder - r * (0.6f * cosf(a)) + up * (0.6f * sinf(a));
        break;
    }
    case 6:   // Spaz: hands in front, shaking everywhere
        R = shoulder + f * 0.35f + r * 0.2f + Vector3(Rand01() - 0.5f, Rand01() - 0.5f, Rand01() - 0.5f) * 0.3f;
        L = shoulder + f * 0.35f - r * 0.2f + Vector3(Rand01() - 0.5f, Rand01() - 0.5f, Rand01() - 0.5f) * 0.3f;
        break;
    case 7:   // Beyblade: hands spin fast around your waist
        R = shoulder - up * 0.3f + r * (0.5f * cosf(t * 20)) + f * (0.5f * sinf(t * 20));
        L = shoulder - up * 0.3f - r * (0.5f * cosf(t * 20)) - f * (0.5f * sinf(t * 20));
        break;
    case 8:   // Superman: both hands stretched out in front of you
        R = shoulder + f * 0.65f + r * 0.1f + up * 0.3f;
        L = shoulder + f * 0.65f - r * 0.1f + up * 0.3f;
        break;
    case 9:   // Flap: arms out to the sides, flapping like wings
        R = shoulder + r * 0.6f + up * (0.45f * sinf(t * 9));
        L = shoulder - r * 0.6f + up * (0.45f * sinf(t * 9));
        break;
    case 10: {  // Boxing: punches one after the other
        float p = sinf(t * 7);
        R = shoulder + f * (0.25f + 0.4f * fmaxf(0.0f, p)) + r * 0.15f;
        L = shoulder + f * (0.25f + 0.4f * fmaxf(0.0f, -p)) - r * 0.15f;
        break;
    }
    case 11: {  // Clap: hands meet in front of you
        float p = 0.5f + 0.5f * cosf(t * 9);
        R = shoulder + f * 0.35f + r * (0.04f + 0.3f * p);
        L = shoulder + f * 0.35f - r * (0.04f + 0.3f * p);
        break;
    }
    case 12:  // Hands Up: both arms over your head, swaying
        R = shoulder + r * (0.25f + 0.05f * s) + up * 0.65f;
        L = shoulder - r * (0.25f + 0.05f * s) + up * 0.65f;
        break;
    case 13: {  // Sprinkler: one hand behind your head, the other sweeps out and back
        float a = sinf(t * 3) * 0.9f;
        R = shoulder + r * (0.55f * cosf(a)) + f * (0.55f * sinf(a)) + up * 0.05f;
        L = shoulder + up * 0.4f - f * 0.1f - r * 0.1f;
        break;
    }
    case 14: {  // Funny Run: arms pump front and back like a sprint
        float p = t * g_funnySpeed;
        R = shoulder + f * (0.4f * sinf(p)) + r * 0.2f + up * (-0.1f + 0.2f * cosf(p));
        L = shoulder - f * (0.4f * sinf(p)) - r * 0.2f + up * (-0.1f - 0.2f * cosf(p));
        break;
    }
    case 15: {  // Spin: both hands circle together in front of you
        float p = t * 7;
        Vector3 c = shoulder + f * 0.4f + up * 0.05f;
        R = c + r * (0.3f * cosf(p)) + up * (0.3f * sinf(p)) + r * 0.05f;
        L = c + r * (0.3f * cosf(p)) + up * (0.3f * sinf(p)) - r * 0.05f;
        break;
    }
    case 16:  // Upside Down: hands hang low and swing, like you are flipped
        R = shoulder + r * 0.25f + up * (-0.55f + 0.05f * s) + f * 0.1f;
        L = shoulder - r * 0.25f + up * (-0.55f - 0.05f * s) + f * 0.1f;
        break;
    case 17: {  // Floss: both arms swing side to side together
        float sway = 0.35f * sinf(t * 8);
        R = shoulder + r * (0.1f + sway) + f * 0.25f - up * 0.2f;
        L = shoulder + r * (-0.1f + sway) + f * 0.25f - up * 0.2f;
        break;
    }
    case 18:  // Dab: one arm out and up, the other bent in front of your face
        R = shoulder + r * 0.55f + up * 0.35f + f * 0.2f;
        L = shoulder + f * 0.3f - r * 0.05f + up * 0.25f;
        break;
    case 19:  // Salute: right hand at your forehead
        R = shoulder + up * 0.42f + f * 0.18f + r * 0.12f;
        L = shoulder - r * 0.22f - up * 0.25f;
        break;
    case 20:  // Zombie: both arms out in front, wobbling
        R = shoulder + f * 0.65f + r * 0.18f + up * (-0.05f + 0.05f * s);
        L = shoulder + f * 0.65f - r * 0.18f + up * (-0.05f - 0.05f * s);
        break;
    case 21: {  // Lasso: right hand spins over your head
        float p = t * 8;
        R = shoulder + up * 0.65f + r * (0.25f * cosf(p)) + f * (0.25f * sinf(p));
        L = shoulder - r * 0.22f - up * 0.25f;
        break;
    }
    case 22:  // Chicken: elbows flapping at your sides
        R = shoulder + r * 0.35f + up * (-0.05f + 0.15f * sinf(t * 14)) - f * 0.05f;
        L = shoulder - r * 0.35f + up * (-0.05f - 0.15f * sinf(t * 14)) - f * 0.05f;
        break;
    case 23: {  // Robot: stiff poses that snap from one to the next
        static const float kRobot[4][3] = {{0.25f, 0.2f, 0.4f}, {0.6f, 0.05f, 0.0f}, {0.15f, -0.1f, 0.5f}, {0.25f, 0.5f, 0.0f}};
        int a = ((int)floorf(t * 2.5f)) & 3, b = (a + 2) & 3;
        R = shoulder + r * kRobot[a][0] + up * kRobot[a][1] + f * kRobot[a][2];
        L = shoulder - r * kRobot[b][0] + up * kRobot[b][1] + f * kRobot[b][2];
        break;
    }
    case 24:  // Heart: hands pulse together in front of your chest
        R = shoulder + f * 0.4f + r * (0.06f + 0.03f * s) - up * 0.1f;
        L = shoulder + f * 0.4f - r * (0.06f + 0.03f * s) - up * 0.1f;
        break;
    case 25:  // Flex: arms bent, fists by your head
        R = shoulder + r * 0.35f + up * (0.35f + 0.03f * s) + f * 0.05f;
        L = shoulder - r * 0.35f + up * (0.35f + 0.03f * s) + f * 0.05f;
        break;
    case 26: {  // Shrug: hands out low and lifting
        float lift = 0.5f + 0.5f * sinf(t * 3);
        R = shoulder + r * 0.45f + f * 0.25f + up * (-0.3f + 0.2f * lift);
        L = shoulder - r * 0.45f + f * 0.25f + up * (-0.3f + 0.2f * lift);
        break;
    }
    case 27: {  // Conductor: the right hand draws a figure eight, the left rests low
        float p = t * 5;
        R = shoulder + f * 0.45f + r * (0.25f * sinf(p)) + up * (0.1f + 0.12f * sinf(2 * p));
        L = shoulder - r * 0.3f + f * 0.2f - up * 0.2f;
        break;
    }
    case 28:  // Both Waving: both hands up, waving out of step
        R = shoulder + r * (0.35f + 0.12f * s) + up * 0.45f + f * 0.1f;
        L = shoulder - r * (0.35f + 0.12f * sinf(t * 6 + 1.5f)) + up * 0.45f + f * 0.1f;
        break;
    case 29: {  // Dead: both arms hang limp, drifting a little
        float d = 0.03f * sinf(t * 1.5f);
        R = shoulder + r * (0.2f + d) - up * (0.62f + 0.02f * sinf(t * 1.2f));
        L = shoulder - r * (0.2f - d) - up * (0.62f - 0.02f * sinf(t * 1.2f));
        break;
    }
    case 31: {  // Collapse: arms rise, then drop and go limp, in a 4 second loop
        float ph = fmodf(t, 4.0f);
        float u = ph < 1.5f ? 0.0f : fminf(1.0f, (ph - 1.5f) / 0.5f);
        float h = 0.5f - 1.12f * u;
        float sway = ph < 1.5f ? 0.05f * s : 0.0f;
        R = shoulder + r * (0.25f + sway) + up * h;
        L = shoulder - r * (0.25f - sway) + up * h;
        break;
    }
    case 30:  // Corpse: arms crossed over the chest, still
        R = shoulder + f * 0.18f - r * 0.08f - up * 0.12f;
        L = shoulder + f * 0.2f + r * 0.08f - up * 0.12f;
        break;
    default:
        if (g_dance >= kGenFirst) GenPose(g_dance - kGenFirst, t, shoulder, r, f, up, R, L);
        else R = L = shoulder;
        break;
    }
    // Keep each hand within arm's reach of its shoulder, then glide there instead of
    // jumping, so the arms move smoothly for you and for everyone watching.
    auto reach = [&](Vector3 hand, Vector3 sh) {
        Vector3 d = hand - sh;
        float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
        return len > 0.7f ? sh + d * (0.7f / len) : hand;
    };
    R = reach(R, shoulder + r * 0.18f);
    L = reach(L, shoulder - r * 0.18f);
    float k = (g_dance == 6 || g_dance == 23) ? 1.0f : fminf(1.0f, dt * 14.0f);     // Spaz stays twitchy
    G.trSetPosition[lh](G.trGetPosition[lh]() + (L - G.trGetPosition[lh]()) * k);
    G.trSetPosition[rh](G.trGetPosition[rh]() + (R - G.trGetPosition[rh]()) * k);
    Quaternion look = Quaternion::LookRotation(f, up);
    G.trSetRotation[lh](look);
    G.trSetRotation[rh](look);
}

// ---- items (everyone sees these) ------------------------------------------------
// Items are made with the game's own item message (RpcCreateItem on the room's
// NetworkMessenger), the same one the game sends when an item appears. Owner-approved
// 2026-10-31. Every way of spawning has a cooldown, and big batches are sent a few per
// frame, so the room can't be flooded.


static void Put(Il2CppObject *arr, size_t i, void *v) { ((void **)((uint8_t *)arr + 0x20))[i] = v; }

// Builds RpcCreateItem's arguments from its own parameter list, in the order the game
// uses: item id, item key, spawn kind 1, no owner, position, rotation, velocity, spin,
// properties, 1, 1.
static bool SendItem(const char *key, Vector3 pos, Quaternion rot, Vector3 vel, char *idOut = nullptr) {
    auto *mi = G.rpcCreateItem;
    Il2CppObject *nm = G.netLocal.IsValid() ? G.netLocal() : nullptr;
    if (!mi || !nm || !G.pvRpc.IsValid() || !G.arrayNew || !G.valueBox || !G.tPhotonView) return false;
    Il2CppObject *go = G.compGameObject[nm]();
    Il2CppObject *pv = go ? G.goGetComponent[go]((Il2CppObject *)G.tPhotonView) : nullptr;
    if (!pv) return false;
    // The last parameter (PhotonMessageInfo) is filled in by Photon on the receiving
    // side; it is never sent.
    int count = mi->parameters_count;
    if (count > 0 && TypeName(mi->parameters[count - 1]) == "Photon.Pun.PhotonMessageInfo") count--;
    auto *args = (Il2CppObject *)G.arrayNew(G.objectClass, count);
    if (!args) return false;
    char id[8];
    static const char kChars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int i = 0; i < 4; i++) id[i] = kChars[(int)(Rand01() * 35.99f)];
    id[4] = 0;
    if (idOut) memcpy(idOut, id, 5);
    int strings = 0, vectors = 0, others = 0;
    for (int i = 0; i < count; i++) {
        std::string tn = TypeName(mi->parameters[i]);
        Class pc(mi->parameters[i]);
        auto *k = pc.IsValid() ? pc.GetClass() : nullptr;
        void *v = nullptr;
        if (tn == "System.String") {
            if (strings == 0) v = CreateMonoString(id);
            else if (strings == 1) v = CreateMonoString(key);
            strings++;                                   // later strings stay null
        } else if (tn == "UnityEngine.Vector3") {
            Vector3 vec = vectors == 0 ? pos : vectors == 1 ? vel : Vector3(0, 0, 0);
            vectors++;
            v = G.valueBox(k, &vec);
        } else if (tn == "UnityEngine.Quaternion") {
            v = G.valueBox(k, &rot);
        } else if (tn == "System.String[]") {
            v = G.arrayNew(G.stringClass, 0);
        } else if (k && k->byval_arg.valuetype) {
            uint8_t raw[64] = {};
            raw[0] = 1;                                 // spawn kind 1 / the two bytes = 1
            // Photon can't send the game's enum types, only plain numbers (like the kit's
            // Int32 1), so an enum goes as its underlying number type.
            auto *boxAs = k->enumtype && k->element_class ? k->element_class : k;
            v = G.valueBox(boxAs, raw);
            others++;
        }
        Put(args, i, v);
    }
    static bool loggedTypes = false;
    if (!loggedTypes) {                                   // once: what the game expects, for fixing
        loggedTypes = true;
        std::string sig;
        for (int i = 0; i < mi->parameters_count; i++) {
            Class pc(mi->parameters[i]);
            auto *k = pc.IsValid() ? pc.GetClass() : nullptr;
            std::string tn = TypeName(mi->parameters[i]);
            if (tn.size() > 30) tn = k && k->byval_arg.valuetype ? (k->enumtype ? "enum" : "struct" + std::to_string(k->instance_size - 16)) : "class";
            sig += (i ? "," : "") + tn;
        }
        ym::log("starter-menu: item message takes %s", sig.c_str());
    }
    try {
        G.pvRpc[pv](CreateMonoString("RpcCreateItem"), 0, args);      // 0 = everyone in the room
    } catch (BNM::IL2CPP::Il2CppExceptionWrapper &w) {   // the game said no: log why, don't crash the menu
        auto *ex = w.ex;
        auto *msg = ex ? (BNM::Structures::Mono::String *)ex->message : nullptr;
        auto *cls = ex ? (BNM::Structures::Mono::String *)ex->className : nullptr;
        ym::log("starter-menu: item message error %s: %s", cls ? cls->str().c_str() : "?", msg ? msg->str().c_str() : "?");
        return false;
    }
    return true;
}

static void QueueItems(const std::vector<PendingItem> &list) {
    if (!g_itemKey[0]) return;
    if (g_time < g_nextBatch) { Notify("Wait a moment"); return; }
    g_nextBatch = g_time + 3.0;
    for (auto &p : list) if (g_itemQueue.size() < 32) g_itemQueue.push_back(p);
}

static void ItemQueueTick() {
    for (int sent = 0; sent < 2 && !g_itemQueue.empty(); sent++) {   // 2 a frame at most
        PendingItem p = g_itemQueue.front();
        g_itemQueue.erase(g_itemQueue.begin());
        bool ok = SendItem(g_itemKey, p.pos, Quaternion(0, 0, 0, 1), p.vel);
        static int logged = 0;
        if (logged++ < 3) ym::log("starter-menu: spawn %s ok=%d", g_itemKey, ok ? 1 : 0);
        if (!ok) { g_itemQueue.clear(); Notify("Item spawning isn't ready in this room"); }
    }
}

// The grid in front of you: 3 across, 3 deep, and 3 high for 3x3x3.
static void SpawnGrid(int count) {
    Il2CppObject *head = g_head.get();
    if (!head) return;
    Vector3 f = Flat(G.trGetForward[head]()), r = Cross(Vector3(0, 1, 0), f);
    Vector3 base = G.trGetPosition[head]() + f * 1.2f + Vector3(0, -0.4f, 0);
    std::vector<PendingItem> list;
    for (int y = 0; y < (count > 9 ? 3 : 1); y++)
        for (int z = 0; z < 3; z++)
            for (int x = 0; x < 3; x++)
                list.push_back({base + r * ((x - 1) * 0.45f) + f * (z * 0.45f) + Vector3(0, y * 0.45f, 0), Vector3(0, 0, 0)});
    QueueItems(list);
}
// The item you're holding in your LEFT hand right now (owner 2026-10-31: items spawn as
// whatever is in your left hand when you press). One scene search per press.
static bool LeftHandItem() {
    Il2CppObject *hand = g_leftHand.get();
    if (!hand || !G.tItem || !G.itemKey.IsValid() || !(G.findScene.IsValid() || G.findSceneByType.IsValid())) return false;
    auto *arr = (uint8_t *)(G.findScene.IsValid() ? G.findScene((Il2CppObject *)G.tItem)
                                                  : G.findSceneByType((Il2CppObject *)G.tItem, 0));
    size_t n = arr ? *(uintptr_t *)(arr + 0x18) : 0;
    Vector3 hp = G.trGetPosition[hand]();
    Il2CppObject *best = nullptr;
    float bestD = 0.4f * 0.4f;
    for (size_t i = 0; i < n; i++) {
        Il2CppObject *it = ((Il2CppObject **)(arr + 0x20))[i];
        if (!Alive(it) || (G.itemHeld.IsValid() && !G.itemHeld[it]())) continue;
        Vector3 d = G.trGetPosition[G.compTransform[it]()]() - hp;
        float dd = d.x * d.x + d.y * d.y + d.z * d.z;
        if (dd < bestD) { bestD = dd; best = it; }
    }
    auto *key = best ? G.itemKey[best]() : nullptr;
    if (!key) { Notify("Hold an item in your left hand"); return false; }
    snprintf(g_itemKey, sizeof g_itemKey, "%s", key->str().c_str());
    return true;
}
static void ArmDupe9() { if (LeftHandItem()) SpawnGrid(9); }
static void ArmDupe27() { if (LeftHandItem()) SpawnGrid(27); }

// More shapes in front of you. Same rules as the grids: the item in your left hand,
// through QueueItems (one batch every 3 s, at most 27 items, 2 a frame).
// The ring is centered 2 m ahead so nothing spawns on top of you.
enum Shape { SHAPE_CIRCLE, SHAPE_SQUARE, SHAPE_TRIANGLE, SHAPE_LINE, SHAPE_SPIRAL, SHAPE_PYRAMID, SHAPE_HEART, SHAPE_CROSS, SHAPE_STAR, SHAPE_DONUT, SHAPE_DIAMOND, SHAPE_ARROW, SHAPE_WAVE, SHAPE_TOWER, SHAPE_WALL, SHAPE_GRID };
static void SpawnShape(Shape shape) {
    Il2CppObject *head = g_head.get();
    if (!head) return;
    Vector3 f = Flat(G.trGetForward[head]()), r = Cross(Vector3(0, 1, 0), f);
    Vector3 base = G.trGetPosition[head]() + f * 1.4f + Vector3(0, -0.4f, 0);
    std::vector<PendingItem> list;
    if (shape == SHAPE_CIRCLE) {                       // 12 around a ring, 0.8 m wide
        Vector3 c = base + f * 0.8f;
        for (int i = 0; i < 12; i++) {
            float a = i * (6.2831853f / 12.0f);
            list.push_back({c + r * (cosf(a) * 0.8f) + f * (sinf(a) * 0.8f), Vector3(0, 0, 0)});
        }
    } else if (shape == SHAPE_SQUARE) {                // 4 across, 4 deep = 16
        for (int z = 0; z < 4; z++)
            for (int x = 0; x < 4; x++)
                list.push_back({base + r * ((x - 1.5f) * 0.45f) + f * (z * 0.45f), Vector3(0, 0, 0)});
    } else if (shape == SHAPE_TRIANGLE) {              // rows of 1, 2, 3, 4, 5 = 15
        for (int row = 0; row < 5; row++)
            for (int i = 0; i <= row; i++)
                list.push_back({base + r * ((i - row * 0.5f) * 0.45f) + f * (row * 0.45f), Vector3(0, 0, 0)});
    } else if (shape == SHAPE_LINE) {                  // 10 side by side
        for (int i = 0; i < 10; i++)
            list.push_back({base + r * ((i - 4.5f) * 0.4f) + f * 0.6f, Vector3(0, 0, 0)});
    } else if (shape == SHAPE_SPIRAL) {                // a spiral of 12 that opens outward
        Vector3 c = base + f * 0.9f;
        for (int i = 0; i < 12; i++) {
            float a = i * 0.9f, rad = 0.2f + i * 0.07f;
            list.push_back({c + r * (cosf(a) * rad) + f * (sinf(a) * rad), Vector3(0, 0, 0)});
        }
    } else if (shape == SHAPE_PYRAMID) {               // a pyramid: 3x3 at the bottom, then 2x2, then 1 = 14
        Vector3 c = base + f * 0.5f;
        const float gap = 0.45f, rise = 0.4f;
        for (int layer = 0; layer < 3; layer++) {
            int n = 3 - layer;                         // 3, 2, 1 across
            for (int z = 0; z < n; z++)
                for (int x = 0; x < n; x++)
                    list.push_back({c + r * ((x - (n - 1) * 0.5f) * gap) + f * ((z - (n - 1) * 0.5f) * gap) + Vector3(0, layer * rise, 0),
                                    Vector3(0, 0, 0)});
        }
    } else if (shape == SHAPE_HEART) {                 // 16 in a heart, the point toward you
        Vector3 c = base + f * 1.0f;
        for (int i = 0; i < 16; i++) {
            float a = i * (6.2831853f / 16.0f);
            float hx = 16.0f * powf(sinf(a), 3.0f);
            float hy = 13.0f * cosf(a) - 5.0f * cosf(2 * a) - 2.0f * cosf(3 * a) - cosf(4 * a);
            list.push_back({c + r * (hx * 0.04f) + f * (hy * 0.04f), Vector3(0, 0, 0)});
        }
    } else if (shape == SHAPE_CROSS) {                 // a plus sign: the middle and two each way = 9
        Vector3 c = base + f * 1.0f;
        list.push_back({c, Vector3(0, 0, 0)});
        for (int i = 1; i <= 2; i++) {
            list.push_back({c + r * (i * 0.45f), Vector3(0, 0, 0)});
            list.push_back({c - r * (i * 0.45f), Vector3(0, 0, 0)});
            list.push_back({c + f * (i * 0.45f), Vector3(0, 0, 0)});
            list.push_back({c - f * (i * 0.45f), Vector3(0, 0, 0)});
        }
    } else if (shape == SHAPE_STAR) {                  // a five-point star outline = 10
        Vector3 c = base + f * 1.0f;
        for (int i = 0; i < 10; i++) {
            float a = i * (6.2831853f / 10.0f), rad = (i % 2 == 0) ? 0.8f : 0.35f;
            list.push_back({c + r * (sinf(a) * rad) + f * (cosf(a) * rad), Vector3(0, 0, 0)});
        }
    } else if (shape == SHAPE_DONUT) {                 // a donut: an outer ring of 16 and an inner ring of 8 = 24
        Vector3 c = base + f * 1.0f;
        for (int i = 0; i < 16; i++) {
            float a = i * (6.2831853f / 16.0f);
            list.push_back({c + r * (cosf(a) * 0.9f) + f * (sinf(a) * 0.9f), Vector3(0, 0, 0)});
        }
        for (int i = 0; i < 8; i++) {
            float a = i * (6.2831853f / 8.0f);
            list.push_back({c + r * (cosf(a) * 0.45f) + f * (sinf(a) * 0.45f), Vector3(0, 0, 0)});
        }
    } else if (shape == SHAPE_DIAMOND) {               // a diamond outline, 3 on each side = 12
        Vector3 c = base + f * 1.2f;
        const float vx[4] = {0.0f, 0.9f, 0.0f, -0.9f}, vy[4] = {0.9f, 0.0f, -0.9f, 0.0f};
        for (int e = 0; e < 4; e++)
            for (int j = 0; j < 3; j++) {
                float k = j / 3.0f, x = vx[e] + (vx[(e + 1) % 4] - vx[e]) * k, y = vy[e] + (vy[(e + 1) % 4] - vy[e]) * k;
                list.push_back({c + r * x + f * y, Vector3(0, 0, 0)});
            }
    } else if (shape == SHAPE_ARROW) {                 // an arrow pointing away from you: a shaft of 4 and a head of 5 = 9
        Vector3 c = base + f * 0.6f;
        for (int i = 0; i < 4; i++) list.push_back({c + f * (i * 0.4f), Vector3(0, 0, 0)});
        for (int sx = -1; sx <= 1; sx += 2) {
            list.push_back({c + r * (sx * 0.7f) + f * 1.5f, Vector3(0, 0, 0)});
            list.push_back({c + r * (sx * 0.35f) + f * 1.85f, Vector3(0, 0, 0)});
        }
        list.push_back({c + f * 2.2f, Vector3(0, 0, 0)});
    } else if (shape == SHAPE_WAVE) {                  // a wavy line across, 12 items
        Vector3 c = base + f * 1.0f;
        for (int i = 0; i < 12; i++)
            list.push_back({c + r * ((i - 5.5f) * 0.35f) + f * (0.4f * sinf(i * 0.8f)), Vector3(0, 0, 0)});
    } else if (shape == SHAPE_TOWER) {                 // a column of 8
        Vector3 c = base + f * 1.0f;
        for (int i = 0; i < 8; i++) list.push_back({c + Vector3(0, i * 0.45f, 0), Vector3(0, 0, 0)});
    } else if (shape == SHAPE_WALL) {                  // a wall, 5 across and 3 high = 15
        Vector3 c = base + f * 1.2f;
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 5; col++)
                list.push_back({c + r * ((col - 2) * 0.45f) + Vector3(0, row * 0.45f, 0), Vector3(0, 0, 0)});
    } else {                                           // a big flat grid, 5 x 5 = 25
        Vector3 c = base + f * 1.6f;
        for (int z = 0; z < 5; z++)
            for (int x = 0; x < 5; x++)
                list.push_back({c + r * ((x - 2) * 0.45f) + f * ((z - 2) * 0.45f), Vector3(0, 0, 0)});
    }
    QueueItems(list);
}
static void ArmCircle() { if (LeftHandItem()) SpawnShape(SHAPE_CIRCLE); }
static void ArmSquare() { if (LeftHandItem()) SpawnShape(SHAPE_SQUARE); }
static void ArmTriangle() { if (LeftHandItem()) SpawnShape(SHAPE_TRIANGLE); }
static void ArmLine() { if (LeftHandItem()) SpawnShape(SHAPE_LINE); }
static void ArmSpiral() { if (LeftHandItem()) SpawnShape(SHAPE_SPIRAL); }
static void ArmPyramid() { if (LeftHandItem()) SpawnShape(SHAPE_PYRAMID); }
static void ArmHeart() { if (LeftHandItem()) SpawnShape(SHAPE_HEART); }
static void ArmCross() { if (LeftHandItem()) SpawnShape(SHAPE_CROSS); }
static void ArmStar() { if (LeftHandItem()) SpawnShape(SHAPE_STAR); }
static void ArmDonut() { if (LeftHandItem()) SpawnShape(SHAPE_DONUT); }
static void ArmDiamond() { if (LeftHandItem()) SpawnShape(SHAPE_DIAMOND); }
static void ArmArrow() { if (LeftHandItem()) SpawnShape(SHAPE_ARROW); }
static void ArmWave() { if (LeftHandItem()) SpawnShape(SHAPE_WAVE); }
static void ArmTower() { if (LeftHandItem()) SpawnShape(SHAPE_TOWER); }
static void ArmWall() { if (LeftHandItem()) SpawnShape(SHAPE_WALL); }
static void ArmBigGrid() { if (LeftHandItem()) SpawnShape(SHAPE_GRID); }
// Only your own waiting items: anything already spawned belongs to the room and stays.
static void ClearItemQueue() {
    int n = (int)g_itemQueue.size();
    g_itemQueue.clear();
    g_dupeArmed = 0;
    Notify("Cleared %d waiting items", n);
}
// ---- Item Multiplier: how many copies one Spawn On Hand press makes ----
static void ItemMultOff() { g_itemMult = 1; Notify("Item multiplier off"); }
static void ItemMult2() { g_itemMult = 2; f_spawnOnHand = true; Notify("Item multiplier x2: hold an item, press right trigger"); }
static void ItemMult3() { g_itemMult = 3; f_spawnOnHand = true; Notify("Item multiplier x3: hold an item, press right trigger"); }
static void ItemMult4() { g_itemMult = 4; f_spawnOnHand = true; Notify("Item multiplier x4: hold an item, press right trigger"); }
static void ItemMult5() { g_itemMult = 5; f_spawnOnHand = true; Notify("Item multiplier x5: hold an item, press right trigger"); }
static void ItemMult6() { g_itemMult = 6; f_spawnOnHand = true; Notify("Item multiplier x6: hold an item, press right trigger"); }
static void ItemMult8() { g_itemMult = 8; f_spawnOnHand = true; Notify("Item multiplier x8: hold an item, press right trigger"); }
static void ItemMult10() { g_itemMult = 10; f_spawnOnHand = true; Notify("Item multiplier x10: hold an item, press right trigger"); }
static void ItemMult12() { g_itemMult = 12; f_spawnOnHand = true; Notify("Item multiplier x12: hold an item, press right trigger"); }
static void ItemMult16() { g_itemMult = 16; f_spawnOnHand = true; Notify("Item multiplier x16: hold an item, press right trigger"); }
static void ItemMult20() { g_itemMult = 20; f_spawnOnHand = true; Notify("Item multiplier x20: hold an item, press right trigger"); }

// ---- Soundboard: sounds only you hear ----
// The sounds are made in code (no files are read) and played by a local AudioSource.
// Everyone Hears It (off to start) also gives the sound to the voice recorder, so the room hears it.
static constexpr int kRate = 22050;
struct SongDef { const char *name; int bpm; int wave; const float *notes; int count; };   // notes: midi, beats, midi, beats...
static const float kSong0[] = {60,1, 60,1, 67,1, 67,1, 69,1, 69,1, 67,2, 65,1, 65,1, 64,1, 64,1, 62,1, 62,1, 60,2, 67,1, 67,1, 65,1, 65,1, 64,1, 64,1, 62,2, 67,1, 67,1, 65,1, 65,1, 64,1, 64,1, 62,2, 60,1, 60,1, 67,1, 67,1, 69,1, 69,1, 67,2, 65,1, 65,1, 64,1, 64,1, 62,1, 62,1, 60,2};
static const float kSong1[] = {64,1, 62,1, 60,1, 62,1, 64,1, 64,1, 64,2, 62,1, 62,1, 62,2, 64,1, 67,1, 67,2, 64,1, 62,1, 60,1, 62,1, 64,1, 64,1, 64,1, 64,1, 62,1, 62,1, 64,1, 62,1, 60,3};
static const float kSong2[] = {64,1, 64,1, 65,1, 67,1, 67,1, 65,1, 64,1, 62,1, 60,1, 60,1, 62,1, 64,1, 64,1.5, 62,0.5, 62,2, 64,1, 64,1, 65,1, 67,1, 67,1, 65,1, 64,1, 62,1, 60,1, 60,1, 62,1, 64,1, 62,1.5, 60,0.5, 60,2};
static const float kSong3[] = {64,1, 64,1, 64,2, 64,1, 64,1, 64,2, 64,1, 67,1, 60,1.5, 62,0.5, 64,4, 65,1, 65,1, 65,1.5, 65,0.5, 65,1, 64,1, 64,1, 64,0.5, 64,0.5, 64,1, 62,1, 62,1, 64,1, 62,2, 67,2};
static const float kSong4[] = {60,1, 60,1, 60,0.75, 62,0.25, 64,1, 64,0.75, 62,0.25, 64,0.75, 65,0.25, 67,2, 72,0.33, 72,0.33, 72,0.34, 67,0.33, 67,0.33, 67,0.34, 64,0.33, 64,0.33, 64,0.34, 60,0.33, 60,0.33, 60,0.34, 67,0.75, 65,0.25, 64,0.75, 62,0.25, 60,2};
static const float kSong5[] = {67,0.75, 67,0.25, 69,1, 67,1, 72,1, 71,2, 67,0.75, 67,0.25, 69,1, 67,1, 74,1, 72,2, 67,0.75, 67,0.25, 79,1, 76,1, 72,1, 71,1, 69,2, 77,0.75, 77,0.25, 76,1, 72,1, 74,1, 72,2};
static const float kSong6[] = {76,0.5, 75,0.5, 76,0.5, 75,0.5, 76,0.5, 71,0.5, 74,0.5, 72,0.5, 69,1.5, 60,0.5, 64,0.5, 69,0.5, 71,1.5, 64,0.5, 68,0.5, 71,0.5, 72,1.5, 64,0.5, 76,0.5, 75,0.5, 76,0.5, 75,0.5, 76,0.5, 71,0.5, 74,0.5, 72,0.5, 69,1.5};
static const float kSong7[] = {60,0.5, 62,0.5, 64,0.5, 65,0.5, 67,0.5, 69,0.5, 71,0.5, 72,1, 71,0.5, 69,0.5, 67,0.5, 65,0.5, 64,0.5, 62,0.5, 60,1};
static const SongDef kSongs[8] = {
    {"Twinkle Star", 120, 0, kSong0, (int)(sizeof(kSong0) / sizeof(float) / 2)},
    {"Mary's Lamb", 132, 0, kSong1, (int)(sizeof(kSong1) / sizeof(float) / 2)},
    {"Ode to Joy", 132, 0, kSong2, (int)(sizeof(kSong2) / sizeof(float) / 2)},
    {"Jingle Bells", 140, 1, kSong3, (int)(sizeof(kSong3) / sizeof(float) / 2)},
    {"Row Your Boat", 110, 0, kSong4, (int)(sizeof(kSong4) / sizeof(float) / 2)},
    {"Happy Birthday", 100, 0, kSong5, (int)(sizeof(kSong5) / sizeof(float) / 2)},
    {"Fur Elise", 130, 0, kSong6, (int)(sizeof(kSong6) / sizeof(float) / 2)},
    {"Scale Run", 180, 2, kSong7, (int)(sizeof(kSong7) / sizeof(float) / 2)},
};
static const char *kSoundNames[kSoundCount] = {"Twinkle Star", "Mary's Lamb", "Ode to Joy", "Jingle Bells", "Row Your Boat", "Happy Birthday", "Fur Elise", "Scale Run", "Beep", "Double Beep", "Siren", "Laser", "Coin", "Power Up", "Power Down", "Drum Roll", "Alarm", "Doorbell", "Bell", "Whoosh", "Heartbeat", "Wobble", "My Song"};

static struct {
    bool tried = false, ok = false;
    BNM::MonoType *tSource = nullptr;
    Method<Il2CppObject *> clipCreate;
    Method<bool> clipSetData;
    Method<void> srcSetClip, srcSetLoop, srcSetVolume, srcPlay, srcStop;
    Ref go, src, clip;
} A;

static bool AudioInit() {
    if (A.ok) return true;
    if (A.tried) return false;
    A.tried = true;
    Class clipC("UnityEngine", "AudioClip"), srcC("UnityEngine", "AudioSource");
    if (!clipC.IsValid() || !srcC.IsValid()) return false;
    A.tSource = srcC.GetMonoType();
    A.clipCreate = Overload(clipC, "Create", {"System.String", "System.Int32", "System.Int32", "System.Int32", "System.Boolean"});
    A.clipSetData = Overload(clipC, "SetData", {"System.Single[]", "System.Int32"});
    A.srcSetClip = Overload(srcC, "set_clip", {"UnityEngine.AudioClip"});
    A.srcSetLoop = Overload(srcC, "set_loop", {"System.Boolean"});
    A.srcSetVolume = Overload(srcC, "set_volume", {"System.Single"});
    A.srcPlay = srcC.GetMethod("Play", 0);
    A.srcStop = srcC.GetMethod("Stop", 0);
    if (!A.clipCreate.IsValid() || !A.clipSetData.IsValid() || !A.srcSetClip.IsValid() || !A.srcSetLoop.IsValid() ||
        !A.srcSetVolume.IsValid() || !A.srcPlay.IsValid() || !A.srcStop.IsValid() || !A.tSource) {
        ym::log("t3wtr4fvr: soundboard missing: create=%d setdata=%d clip=%d loop=%d vol=%d play=%d stop=%d",
                A.clipCreate.IsValid(), A.clipSetData.IsValid(), A.srcSetClip.IsValid(), A.srcSetLoop.IsValid(),
                A.srcSetVolume.IsValid(), A.srcPlay.IsValid(), A.srcStop.IsValid());
        return false;
    }
    Il2CppObject *go = Class("UnityEngine", "GameObject").CreateNewObjectParameters(CreateMonoString("T3wtr4fvrSound"));
    Il2CppObject *src = go ? G.goAddComponent[go](A.tSource) : nullptr;
    if (!go || !src) return false;
    A.go.set(go);
    A.src.set(src);
    A.ok = true;
    return true;
}
static void SoundShutdown() {
    Kill(A.go);
    A.src.clear();
    A.clip.clear();
    A.ok = false;
    A.tried = false;
}


// ---- Everyone Hears It ----
// With the toggle on, a sound is also handed to the game's voice recorder (Photon Voice) in place of
// your microphone, so the room hears it. The owner OK'd soundboards everyone can hear on Discord
// (2026-11-03). Limits: one sound every 3 seconds, 60 seconds at most, never louder than 70%.
// Your microphone is switched off while a sound plays and comes back when it ends or you press Stop.
static struct {
    bool tried = false, ok = false, active = false;
    BNM::MonoType *tRec = nullptr;
    Method<void> setSourceType, setAudioClip, setLoopClip, restart1, restart0;
    Method<int> getSourceType;
    Ref rec;
    int oldSource = 0;
    double restoreAt = 0;
} VC;
static double g_nextVoice = 0;

static bool VoiceInit() {
    if (VC.ok) return true;
    if (VC.tried) return false;
    VC.tried = true;
    Class rc("Photon.Voice.Unity", "Recorder");
    if (!rc.IsValid()) return false;
    VC.tRec = rc.GetMonoType();
    VC.setSourceType = rc.GetMethod("set_SourceType", 1);
    VC.getSourceType = rc.GetMethod("get_SourceType", 0);
    VC.setAudioClip = rc.GetMethod("set_AudioClip", 1);
    VC.setLoopClip = rc.GetMethod("set_LoopAudioClip", 1);
    VC.restart1 = rc.GetMethod("RestartRecording", 1);
    VC.restart0 = rc.GetMethod("RestartRecording", 0);
    VC.ok = VC.tRec && VC.setSourceType.IsValid() && VC.setAudioClip.IsValid() && (VC.restart1.IsValid() || VC.restart0.IsValid());
    ym::log("t3wtr4fvr: voice recorder %d (sourceType %d, clip %d, loop %d, restart %d/%d)", VC.ok, VC.setSourceType.IsValid(),
            VC.setAudioClip.IsValid(), VC.setLoopClip.IsValid(), VC.restart1.IsValid(), VC.restart0.IsValid());
    return VC.ok;
}
static Il2CppObject *VoiceRecorder() {
    if (Alive(VC.rec.get())) return VC.rec.get();
    if (!VC.tRec || !(G.findScene.IsValid() || G.findSceneByType.IsValid())) return nullptr;
    auto *arr = (uint8_t *)(G.findScene.IsValid() ? G.findScene((Il2CppObject *)VC.tRec)
                                                  : G.findSceneByType((Il2CppObject *)VC.tRec, 0));
    size_t n = arr ? *(uintptr_t *)(arr + 0x18) : 0;
    if (!n) return nullptr;
    VC.rec.set(((Il2CppObject **)(arr + 0x20))[0]);
    return VC.rec.get();
}
static void VoiceRestart(Il2CppObject *rec) {
    if (VC.restart1.IsValid()) VC.restart1[rec](true);
    else if (VC.restart0.IsValid()) VC.restart0[rec]();
}
static bool VoiceStart(Il2CppObject *clip, float seconds) {
    if (!VoiceInit()) return false;
    Il2CppObject *rec = VoiceRecorder();
    if (!rec) return false;
    if (!VC.active) VC.oldSource = VC.getSourceType.IsValid() ? VC.getSourceType[rec]() : 0;   // usually 0, the microphone
    VC.setAudioClip[rec](clip);
    if (VC.setLoopClip.IsValid()) VC.setLoopClip[rec](f_soundLoop);
    VC.setSourceType[rec](1);                           // 1 = play an audio clip instead of the microphone
    VoiceRestart(rec);
    VC.active = true;
    VC.restoreAt = f_soundLoop ? 0.0 : g_time + seconds + 0.5;
    return true;
}
static void VoiceStop() {
    if (!VC.active) return;
    VC.active = false;
    VC.restoreAt = 0;
    Il2CppObject *rec = VC.rec.get();
    if (!Alive(rec) || !VC.ok) return;
    if (VC.setLoopClip.IsValid()) VC.setLoopClip[rec](false);
    VC.setSourceType[rec](VC.oldSource);                // your microphone again
    VoiceRestart(rec);
}
static void VoiceTick() {
    if (VC.active && VC.restoreAt > 0 && g_time >= VC.restoreAt) VoiceStop();
}

static float NoteHz(float midi) { return 440.0f * powf(2.0f, (midi - 69.0f) / 12.0f); }
static void AddRest(std::vector<float> &o, float secs) { o.resize(o.size() + (size_t)(secs * kRate), 0.0f); }
static void AddTone(std::vector<float> &o, float hz, float secs, float amp, int wave) {
    int n = (int)(secs * kRate), att = kRate / 100, rel = kRate / 20;
    for (int i = 0; i < n; i++) {
        float ph = (float)fmod((double)i * hz / kRate, 1.0), v;
        if (wave == 0) v = sinf(6.2831853f * ph);
        else if (wave == 1) v = ph < 0.5f ? 0.6f : -0.6f;
        else v = (2.0f * ph - 1.0f) * 0.7f;
        float env = fminf(1.0f, (float)i / att) * fminf(1.0f, (float)(n - i) / rel);
        o.push_back(v * amp * env);
    }
}
static uint32_t g_seed = 12345;
static float Noise() { g_seed = g_seed * 1664525u + 1013904223u; return ((g_seed >> 9) & 0xFFFF) / 32768.0f - 1.0f; }

static void SynthSong(int s, std::vector<float> &o) {
    static const float kAmp[3] = {0.4f, 0.28f, 0.3f};
    const SongDef &d = kSongs[s];
    float beat = 60.0f / d.bpm;
    for (int i = 0; i < d.count; i++) {
        float midi = d.notes[i * 2], len = d.notes[i * 2 + 1] * beat;
        if (midi < 1.0f) { AddRest(o, len); continue; }
        AddTone(o, NoteHz(midi), len * 0.92f, kAmp[d.wave], d.wave);
        AddRest(o, len * 0.08f);
    }
}
static void SynthEffect(int e, std::vector<float> &o) {
    switch (e) {
    case 0: AddTone(o, 880, 0.25f, 0.4f, 0); break;                                                      // Beep
    case 1: AddTone(o, 880, 0.12f, 0.4f, 0); AddRest(o, 0.08f); AddTone(o, 880, 0.12f, 0.4f, 0); break;   // Double Beep
    case 2: {                                                                                            // Siren, 2 seconds
        double ph = 0;
        int n = 2 * kRate;
        for (int i = 0; i < n; i++) {
            double t = (double)i / kRate, hz = 900.0 + 300.0 * sin(6.2831853 * 1.5 * t);
            ph += hz / kRate;
            o.push_back(0.35f * (float)sin(6.2831853 * ph) * fminf(1.0f, fminf((float)i / 441.0f, (float)(n - i) / 2205.0f)));
        }
        break;
    }
    case 3: {                                                                                            // Laser: a quick sweep down
        double ph = 0;
        int n = (int)(0.4 * kRate);
        for (int i = 0; i < n; i++) {
            double k = (double)i / n;
            ph += (1800.0 - 1600.0 * k) / kRate;
            o.push_back((fmod(ph, 1.0) < 0.5 ? 0.25f : -0.25f) * (1.0f - (float)k));
        }
        break;
    }
    case 4: AddTone(o, 988, 0.08f, 0.35f, 1); AddTone(o, 1319, 0.45f, 0.35f, 1); break;                  // Coin
    case 5: {                                                                                            // Power Up
        const float notes[5] = {60, 64, 67, 72, 76};
        for (float m : notes) AddTone(o, NoteHz(m), 0.08f, 0.35f, 1);
        AddTone(o, NoteHz(79), 0.25f, 0.35f, 1);
        break;
    }
    case 6: {                                                                                            // Power Down
        const float notes[5] = {76, 72, 67, 64, 60};
        for (float m : notes) AddTone(o, NoteHz(m), 0.09f, 0.35f, 1);
        AddTone(o, NoteHz(55), 0.3f, 0.35f, 1);
        break;
    }
    case 7: {                                                                                            // Drum Roll
        int n = (int)(1.5 * kRate);
        for (int i = 0; i < n; i++) {
            float t = (float)i / kRate, pulse = fmodf(t, 0.05f) / 0.05f;
            o.push_back(Noise() * 0.5f * (1.0f - pulse) * (0.3f + 0.7f * (float)i / n));
        }
        break;
    }
    case 8: for (int i = 0; i < 10; i++) AddTone(o, (i & 1) ? 600.0f : 800.0f, 0.2f, 0.35f, 1); break;    // Alarm
    case 9: AddTone(o, 659, 0.5f, 0.4f, 0); AddTone(o, 523, 0.9f, 0.4f, 0); break;                       // Doorbell
    case 10: {                                                                                           // Bell
        int n = 2 * kRate;
        for (int i = 0; i < n; i++) {
            double t = (double)i / kRate;
            o.push_back((float)(sin(6.2831853 * 880 * t) + 0.5 * sin(6.2831853 * 2640 * t)) * 0.3f * expf(-3.0f * (float)t));
        }
        break;
    }
    case 11: {                                                                                           // Whoosh
        int n = kRate;
        float lp = 0;
        for (int i = 0; i < n; i++) {
            float k = (float)i / n;
            lp += (Noise() - lp) * (0.05f + 0.4f * k);
            o.push_back(lp * 1.5f * sinf(3.14159265f * k));
        }
        break;
    }
    case 12: {                                                                                           // Heartbeat: four double thumps
        for (int b = 0; b < 4; b++)
            for (int th = 0; th < 2; th++) {
                int n = (int)(0.18 * kRate);
                for (int i = 0; i < n; i++) {
                    double t = (double)i / kRate;
                    o.push_back((float)sin(6.2831853 * 60 * t) * 0.7f * expf(-18.0f * (float)t));
                }
                AddRest(o, th == 0 ? 0.12f : 0.4f);
            }
        break;
    }
    default: {                                                                                           // Wobble: a low buzz that pulses
        int n = 2 * kRate;
        for (int i = 0; i < n; i++) {
            double t = (double)i / kRate;
            o.push_back((float)(2.0 * fmod(t * 110.0, 1.0) - 1.0) * 0.35f * (0.6f + 0.4f * (float)sin(6.2831853 * 6 * t)));
        }
        break;
    }
    }
}
// My Song: your own tune. Type the notes here and rebuild.
// Each note is the letter, the octave and the beats, like C4:1 or G4:2. A sharp is F#4:1 and a rest is R:1.
static const char *kMySong = "E4:1 E4:1 F4:1 G4:1 G4:1 F4:1 E4:1 D4:1 C4:2 R:1";   // a placeholder: replace it with your tune
static const int kMyBpm = 120;
static void SynthMySong(std::vector<float> &o) {
    static const int kSemi[7] = {9, 11, 0, 2, 4, 5, 7};          // A B C D E F G
    float beat = 60.0f / kMyBpm;
    const char *p = kMySong;
    while (*p && o.size() < (size_t)(60 * kRate)) {               // 60 seconds at most
        while (*p == ' ') p++;
        if (!*p) break;
        int semi = -1, oct = 4;
        if (*p == 'R') p++;
        else {
            if (*p >= 'A' && *p <= 'G') semi = kSemi[*p - 'A'];
            p++;
            if (*p == '#') { semi++; p++; }
            if (*p >= '0' && *p <= '9') { oct = *p - '0'; p++; }
        }
        float beats = 1.0f;
        if (*p == ':') { p++; beats = (float)atof(p); while ((*p >= '0' && *p <= '9') || *p == '.') p++; }
        else while (*p && *p != ' ') p++;
        float len = beats * beat;
        if (semi < 0) { AddRest(o, len); continue; }
        AddTone(o, NoteHz((float)(12 * (oct + 1) + semi)), len * 0.92f, 0.4f, 0);
        AddRest(o, len * 0.08f);
    }
}
static void UpdateSoundLabel() {
    snprintf(g_playLabel, sizeof g_playLabel, "Play %d/%d: %s", g_soundIdx + 1, kSoundCount, kSoundNames[g_soundIdx]);
    g_dirty = true;
}
static void PlaySamples(const std::vector<float> &samples, int rate, const char *name);
static void PlayIdx(int idx) {
    try {
        if (!AudioInit()) { Notify("Sound isn't available in this game"); return; }
        std::vector<float> samples;
        if (idx == 22) SynthMySong(samples);
        else if (idx < 8) SynthSong(idx, samples);
        else SynthEffect(idx - 8, samples);
        PlaySamples(samples, kRate, kSoundNames[idx]);
    } catch (...) {
        Notify("Couldn't play the sound");
    }
}
static void PlaySelected() { PlayIdx(g_soundIdx); }
static void NextSong() { g_soundIdx = (g_soundIdx + 1) % kSoundCount; UpdateSoundLabel(); }
static void PrevSong() { g_soundIdx = (g_soundIdx + kSoundCount - 1) % kSoundCount; UpdateSoundLabel(); }
static void StopSound() {
    VoiceStop();
    if (A.ok && Alive(A.src.get())) A.srcStop[A.src.get()]();
    Notify("Stopped");
}
static void ApplyShare() { if (!f_shareVoice) VoiceStop(); }
static void ApplyLoop() { if (A.ok && Alive(A.src.get())) A.srcSetLoop[A.src.get()](f_soundLoop); }
static void ApplyVolume() {
    if (A.ok && Alive(A.src.get())) A.srcSetVolume[A.src.get()](g_soundVol);
    Notify("Volume %d%%", (int)(g_soundVol * 100 + 0.5f));
}
static void VolUp() { g_soundVol = fminf(1.0f, g_soundVol + 0.1f); ApplyVolume(); }
static void VolDown() { g_soundVol = fmaxf(0.1f, g_soundVol - 0.1f); ApplyVolume(); }
static void RandomSound() { g_soundIdx = (int)(g_time * 1000.0) % kSoundCount; UpdateSoundLabel(); PlayIdx(g_soundIdx); }

// ---- My WAVs: your own .wav files, played on your own headset only ----
// Put .wav files in Android/data/com.TrassGames.Yeeps/files/sounds/ (next to the mods folder).
// The file is read, turned into samples and played by the same local AudioSource as the Soundboard.
// Everyone Hears It works here too. 16-bit, 8-bit, 24-bit and float WAVs work.
static const char *kWavDir = "/storage/emulated/0/Android/data/com.TrassGames.Yeeps/files/sounds/";
static std::vector<std::string> g_wavs;
static int g_wavIdx = 0;
static const size_t kMaxWavSamples = 8000000;      // about 6 minutes at 22 kHz

static void PlaySamples(const std::vector<float> &samples, int rate, const char *name) {
    if (!AudioInit()) { Notify("Sound isn't available in this game"); return; }
    if (samples.empty()) { Notify("That file is empty"); return; }
    const std::vector<float> *use = &samples;
    std::vector<float> limited;
    bool share = f_shareVoice;
    if (share) {
        if (g_time < g_nextVoice) { Notify("Wait a moment"); return; }          // one sound every 3 seconds for the room
        float peak = 0;
        for (float v : samples) peak = fmaxf(peak, fabsf(v));
        float gain = peak > 0.7f ? 0.7f / peak : 1.0f;                           // never louder than 70% for the room
        size_t maxN = (size_t)(60 * rate);                                       // 60 seconds at most for the room
        size_t n = samples.size() < maxN ? samples.size() : maxN;
        limited.assign(samples.begin(), samples.begin() + n);
        if (gain < 1.0f) for (auto &v : limited) v *= gain;
        use = &limited;
    }
    Il2CppObject *clip = A.clipCreate(CreateMonoString("T3Clip"), (int)use->size(), 1, rate, false);
    if (!clip) { Notify("Couldn't make the sound"); return; }
    auto *arr = BNM::Structures::Mono::Array<float>::Create(*use);
    A.clipSetData[clip](arr, 0);
    A.clip.set(clip);
    Il2CppObject *src = A.src.get();
    A.srcSetClip[src](clip);
    A.srcSetLoop[src](f_soundLoop);
    A.srcSetVolume[src](share && !f_hearSelf ? 0.0f : g_soundVol);              // Hear It Yourself off: only the room hears it
    A.srcPlay[src]();
    if (share) {
        if (VoiceStart(clip, (float)use->size() / rate)) {
            g_nextVoice = g_time + 3.0;
            Notify("Everyone hears: %s", name);
            return;
        }
        Notify("Voice chat isn't available, so only you hear it");
        return;
    }
    Notify("Playing %s", name);
}
static void UpdateWavLabel() {
    if (g_wavs.empty()) { snprintf(g_wavLabel, sizeof g_wavLabel, "Play WAV: none found"); g_dirty = true; return; }
    std::string stem = g_wavs[g_wavIdx];
    if (stem.size() > 4) stem.resize(stem.size() - 4);                  // drop ".wav"
    snprintf(g_wavLabel, sizeof g_wavLabel, "Play %d/%d: %.28s", g_wavIdx + 1, (int)g_wavs.size(), stem.c_str());
    g_dirty = true;
}
static void WavScan() {
    g_wavs.clear();
    if (DIR *d = opendir(kWavDir)) {
        while (dirent *e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".wav") == 0) g_wavs.push_back(n);
        }
        closedir(d);
    }
    std::sort(g_wavs.begin(), g_wavs.end());
    if (g_wavIdx >= (int)g_wavs.size()) g_wavIdx = 0;
    UpdateWavLabel();
    if (g_wavs.empty()) Notify("No .wav files in the sounds folder");
    else Notify("%d .wav files found", (int)g_wavs.size());
}
// Reads a PCM or float WAV into mono samples. Loud-rate files (32 kHz and up) are halved to save memory.
static bool LoadWav(const std::string &path, std::vector<float> &out, int &rate) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) { fclose(f); return false; }
    int tag = 0, ch = 0, bits = 0;
    uint32_t dataLen = 0;
    bool haveData = false;
    rate = 0;
    for (int chunk = 0; chunk < 64; chunk++) {              // look through the first 64 chunks for fmt and data
        char id[4];
        uint32_t sz = 0;
        if (fread(id, 1, 4, f) != 4 || fread(&sz, 4, 1, f) != 1) break;
        if (memcmp(id, "fmt ", 4) == 0) {
            uint8_t b[16] = {0};
            size_t n = sz < 16 ? sz : 16;
            if (fread(b, 1, n, f) != n) break;
            tag = b[0] | (b[1] << 8);
            ch = b[2] | (b[3] << 8);
            rate = b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24);
            bits = b[14] | (b[15] << 8);
            fseek(f, (long)(sz - n) + (long)(sz & 1), SEEK_CUR);
        } else if (memcmp(id, "data", 4) == 0) {
            dataLen = sz;
            haveData = true;
            break;
        } else {
            fseek(f, (long)sz + (long)(sz & 1), SEEK_CUR);
        }
    }
    bool floatData = (tag == 3 && bits == 32);
    bool pcm = (tag == 1 || tag == 0xFFFE) && (bits == 8 || bits == 16 || bits == 24);
    if (!haveData || ch < 1 || ch > 8 || rate < 4000 || (!pcm && !floatData)) { fclose(f); return false; }
    int bps = bits / 8, frameSize = ch * bps;
    int decimate = rate >= 32000 ? 2 : 1;
    long remaining = (long)(dataLen / frameSize);
    std::vector<uint8_t> buf((size_t)frameSize * 4096);
    float acc = 0;
    int accN = 0;
    while (remaining > 0 && out.size() < kMaxWavSamples) {
        size_t want = remaining < 4096 ? (size_t)remaining : 4096;
        size_t got = fread(buf.data(), (size_t)frameSize, want, f);
        if (got == 0) break;
        for (size_t i = 0; i < got; i++) {
            float m = 0;
            for (int c = 0; c < ch; c++) {
                const uint8_t *q = buf.data() + i * (size_t)frameSize + (size_t)c * bps;
                float v;
                if (floatData) { float x; memcpy(&x, q, 4); v = x; }
                else if (bits == 16) v = (int16_t)(q[0] | (q[1] << 8)) / 32768.0f;
                else if (bits == 24) v = (int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) / 2147483648.0f;
                else v = ((int)q[0] - 128) / 128.0f;
                m += v;
            }
            acc += m / ch;
            if (++accN == decimate) { out.push_back(acc / accN); acc = 0; accN = 0; }
        }
        remaining -= (long)got;
    }
    fclose(f);
    rate /= decimate;
    return !out.empty();
}
static void WavPlay() {
    try {
        if (g_wavs.empty()) WavScan();
        if (g_wavs.empty()) return;
        std::vector<float> samples;
        int rate = 22050;
        if (!LoadWav(std::string(kWavDir) + g_wavs[g_wavIdx], samples, rate)) { Notify("Couldn't read that WAV (try 16-bit PCM)"); return; }
        PlaySamples(samples, rate, g_wavs[g_wavIdx].c_str());
    } catch (...) {
        Notify("Couldn't play that WAV");
    }
}
static void WavNext() {
    if (g_wavs.empty()) { WavScan(); return; }
    g_wavIdx = (g_wavIdx + 1) % (int)g_wavs.size();
    UpdateWavLabel();
}
static void WavPrev() {
    if (g_wavs.empty()) { WavScan(); return; }
    g_wavIdx = (g_wavIdx + (int)g_wavs.size() - 1) % (int)g_wavs.size();
    UpdateWavLabel();
}

// ---- Item Text: type a word, then spell it with items ----
// A 3x5 dot font. The word is split into lines of at most 27 items (the batch limit). Each press of
// "Spawn Next Line" spawns one line of the word, using the item in your left hand, in front of you
// and flat on the ground. The first line is the farthest, so the word reads from where you stand.
// One batch every 3 seconds, like every other shape. Nothing spawns by itself.
static const char *kGlyph[36][5] = {
    {"010", "101", "111", "101", "101"},  // A
    {"110", "101", "110", "101", "110"},  // B
    {"011", "100", "100", "100", "011"},  // C
    {"110", "101", "101", "101", "110"},  // D
    {"111", "100", "110", "100", "111"},  // E
    {"111", "100", "110", "100", "100"},  // F
    {"011", "100", "101", "101", "011"},  // G
    {"101", "101", "111", "101", "101"},  // H
    {"111", "010", "010", "010", "111"},  // I
    {"001", "001", "001", "101", "010"},  // J
    {"101", "101", "110", "101", "101"},  // K
    {"100", "100", "100", "100", "111"},  // L
    {"101", "111", "111", "101", "101"},  // M
    {"110", "101", "101", "101", "101"},  // N
    {"010", "101", "101", "101", "010"},  // O
    {"110", "101", "110", "100", "100"},  // P
    {"010", "101", "101", "110", "011"},  // Q
    {"110", "101", "110", "101", "101"},  // R
    {"011", "100", "010", "001", "110"},  // S
    {"111", "010", "010", "010", "010"},  // T
    {"101", "101", "101", "101", "111"},  // U
    {"101", "101", "101", "101", "010"},  // V
    {"101", "101", "111", "111", "101"},  // W
    {"101", "101", "010", "101", "101"},  // X
    {"101", "101", "010", "010", "010"},  // Y
    {"111", "001", "010", "100", "111"},  // Z
    {"111", "101", "101", "101", "111"},  // 0
    {"010", "110", "010", "010", "111"},  // 1
    {"110", "001", "010", "100", "111"},  // 2
    {"110", "001", "010", "001", "110"},  // 3
    {"101", "101", "111", "001", "001"},  // 4
    {"111", "100", "110", "001", "110"},  // 5
    {"011", "100", "111", "101", "111"},  // 6
    {"111", "001", "010", "010", "010"},  // 7
    {"111", "101", "111", "101", "111"},  // 8
    {"111", "101", "111", "001", "110"},  // 9
};
static std::string g_text;
static int g_textNext = 0;                         // the next line to spawn
static constexpr size_t kMaxText = 20;

static int GlyphIndex(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    return -1;
}
static int GlyphCount(char c) {
    int gi = GlyphIndex(c), n = 0;
    if (gi < 0) return 0;
    for (int row = 0; row < 5; row++)
        for (int col = 0; col < 3; col++)
            if (kGlyph[gi][row][col] == '1') n++;
    return n;
}
static void SplitText(std::vector<std::string> &lines) {
    lines.clear();
    std::string cur;
    int items = 0;
    for (char c : g_text) {
        if (cur.empty() && c == ' ') continue;                          // no spaces at the start of a line
        int n = GlyphCount(c);
        if (items + n > 27 && !cur.empty()) { lines.push_back(cur); cur.clear(); items = 0; if (c == ' ') continue; }
        cur += c;
        items += n;
    }
    while (!cur.empty() && cur.back() == ' ') cur.pop_back();
    if (!cur.empty()) lines.push_back(cur);
}
static void UpdateTextLabels() {
    std::string shown = g_text.empty() ? std::string("(empty)") : g_text;
    if (shown.size() > 22) shown = shown.substr(shown.size() - 22);
    snprintf(g_textLabel, sizeof g_textLabel, "Text: %s", shown.c_str());
    std::vector<std::string> lines;
    SplitText(lines);
    snprintf(g_textSpawnLabel, sizeof g_textSpawnLabel, "Spawn Next Line (%d/%d)", std::min(g_textNext, (int)lines.size()), (int)lines.size());
    g_dirty = true;
}
static void TextAdd(char c) {
    if (g_text.size() >= kMaxText) { Notify("%d characters at most", (int)kMaxText); return; }
    g_text += c;
    g_textNext = 0;
    UpdateTextLabels();
}
static void TextShow() { Notify("Your text: %s", g_text.empty() ? "(empty)" : g_text.c_str()); }
static void TextSpace() { TextAdd(' '); }
static void TextBack() { if (!g_text.empty()) g_text.pop_back(); g_textNext = 0; UpdateTextLabels(); }
static void TextClear() { g_text.clear(); g_textNext = 0; UpdateTextLabels(); }
static void TextRestart() { g_textNext = 0; UpdateTextLabels(); Notify("Starting over"); }
static void TextPreset() { g_text = "HEARTVRONTOP IS A W"; g_textNext = 0; UpdateTextLabels(); }
static void TextSpawnNext() {
    std::vector<std::string> lines;
    SplitText(lines);
    if (lines.empty()) { Notify("Type something first"); return; }
    if (g_textNext >= (int)lines.size()) { Notify("All lines spawned. Press Restart Text to go again"); return; }
    Il2CppObject *head = g_head.get();
    if (!head) return;
    if (g_time < g_nextBatch) { Notify("Wait a moment"); return; }          // one batch every 3 seconds
    if (!LeftHandItem()) return;
    const std::string &line = lines[g_textNext];
    const float dot = 0.22f;
    int cols = (int)line.size() * 4 - 1;
    Vector3 f = Flat(G.trGetForward[head]()), r = Cross(Vector3(0, 1, 0), f);
    float farthest = 2.5f + ((int)lines.size() - 1) * 1.6f, dist = farthest - g_textNext * 1.6f;   // the first line is the farthest
    Vector3 base = G.trGetPosition[head]() + f * dist + Vector3(0, -0.4f, 0);
    std::vector<PendingItem> list;
    for (size_t k = 0; k < line.size(); k++) {
        int gi = GlyphIndex(line[k]);
        if (gi < 0) continue;
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (kGlyph[gi][row][col] == '1')
                    list.push_back({base + r * ((((int)k * 4 + col) - cols * 0.5f) * dot) + f * ((4 - row) * dot), Vector3(0, 0, 0)});
    }
    QueueItems(list);
    g_textNext++;
    UpdateTextLabels();
}
static void KeyA() { TextAdd('A'); }
static void KeyB() { TextAdd('B'); }
static void KeyC() { TextAdd('C'); }
static void KeyD() { TextAdd('D'); }
static void KeyE() { TextAdd('E'); }
static void KeyF() { TextAdd('F'); }
static void KeyG() { TextAdd('G'); }
static void KeyH() { TextAdd('H'); }
static void KeyI() { TextAdd('I'); }
static void KeyJ() { TextAdd('J'); }
static void KeyK() { TextAdd('K'); }
static void KeyL() { TextAdd('L'); }
static void KeyM() { TextAdd('M'); }
static void KeyN() { TextAdd('N'); }
static void KeyO() { TextAdd('O'); }
static void KeyP() { TextAdd('P'); }
static void KeyQ() { TextAdd('Q'); }
static void KeyR() { TextAdd('R'); }
static void KeyS() { TextAdd('S'); }
static void KeyT() { TextAdd('T'); }
static void KeyU() { TextAdd('U'); }
static void KeyV() { TextAdd('V'); }
static void KeyW() { TextAdd('W'); }
static void KeyX() { TextAdd('X'); }
static void KeyY() { TextAdd('Y'); }
static void KeyZ() { TextAdd('Z'); }
static void Key0() { TextAdd('0'); }
static void Key1() { TextAdd('1'); }
static void Key2() { TextAdd('2'); }
static void Key3() { TextAdd('3'); }
static void Key4() { TextAdd('4'); }
static void Key5() { TextAdd('5'); }
static void Key6() { TextAdd('6'); }
static void Key7() { TextAdd('7'); }
static void Key8() { TextAdd('8'); }
static void Key9() { TextAdd('9'); }
static void BorderStyle0() { g_borderStyle = 0; g_dirty = true; Notify("Border: Off"); }
static void BorderStyle1() { g_borderStyle = 1; g_dirty = true; Notify("Border: Stripes"); }
static void BorderStyle2() { g_borderStyle = 2; g_dirty = true; Notify("Border: Solid"); }
static void BorderStyle3() { g_borderStyle = 3; g_dirty = true; Notify("Border: Dots"); }
static void BorderStyle4() { g_borderStyle = 4; g_dirty = true; Notify("Border: Lights"); }
static void BorderStyle5() { g_borderStyle = 5; g_dirty = true; Notify("Border: Zigzag"); }
static void BorderStyle6() { g_borderStyle = 6; g_dirty = true; Notify("Border: Checker"); }
static void BorderColor0() { g_borderColor = 0; g_dirty = true; Notify("Border color: Match Theme"); }
static void BorderColor1() { g_borderColor = 1; g_dirty = true; Notify("Border color: Red and White"); }
static void BorderColor2() { g_borderColor = 2; g_dirty = true; Notify("Border color: Orange and Black"); }
static void BorderColor3() { g_borderColor = 3; g_dirty = true; Notify("Border color: Green and Red"); }
static void BorderColor4() { g_borderColor = 4; g_dirty = true; Notify("Border color: Pink and White"); }
static void BorderColor5() { g_borderColor = 5; g_dirty = true; Notify("Border color: Gold and Black"); }
static void BorderColor6() { g_borderColor = 6; g_dirty = true; Notify("Border color: Blue and White"); }
static void BorderColor7() { g_borderColor = 7; g_dirty = true; Notify("Border color: Mint and White"); }
static void BorderColor8() { g_borderColor = 8; g_dirty = true; Notify("Border color: Purple and Gold"); }
static void BorderColor9() { g_borderColor = 9; g_dirty = true; Notify("Border color: Teal and Coral"); }
static void BorderColor10() { g_borderColor = 10; g_dirty = true; Notify("Border color: Black and White"); }
static void BorderColor11() { g_borderColor = 11; g_dirty = true; Notify("Border color: Rainbow"); }
static void Snd0() { g_soundIdx = 0; UpdateSoundLabel(); PlayIdx(0); }
static void Snd1() { g_soundIdx = 1; UpdateSoundLabel(); PlayIdx(1); }
static void Snd2() { g_soundIdx = 2; UpdateSoundLabel(); PlayIdx(2); }
static void Snd3() { g_soundIdx = 3; UpdateSoundLabel(); PlayIdx(3); }
static void Snd4() { g_soundIdx = 4; UpdateSoundLabel(); PlayIdx(4); }
static void Snd5() { g_soundIdx = 5; UpdateSoundLabel(); PlayIdx(5); }
static void Snd6() { g_soundIdx = 6; UpdateSoundLabel(); PlayIdx(6); }
static void Snd7() { g_soundIdx = 7; UpdateSoundLabel(); PlayIdx(7); }
static void Snd8() { g_soundIdx = 8; UpdateSoundLabel(); PlayIdx(8); }
static void Snd9() { g_soundIdx = 9; UpdateSoundLabel(); PlayIdx(9); }
static void Snd10() { g_soundIdx = 10; UpdateSoundLabel(); PlayIdx(10); }
static void Snd11() { g_soundIdx = 11; UpdateSoundLabel(); PlayIdx(11); }
static void Snd12() { g_soundIdx = 12; UpdateSoundLabel(); PlayIdx(12); }
static void Snd13() { g_soundIdx = 13; UpdateSoundLabel(); PlayIdx(13); }
static void Snd14() { g_soundIdx = 14; UpdateSoundLabel(); PlayIdx(14); }
static void Snd15() { g_soundIdx = 15; UpdateSoundLabel(); PlayIdx(15); }
static void Snd16() { g_soundIdx = 16; UpdateSoundLabel(); PlayIdx(16); }
static void Snd17() { g_soundIdx = 17; UpdateSoundLabel(); PlayIdx(17); }
static void Snd18() { g_soundIdx = 18; UpdateSoundLabel(); PlayIdx(18); }
static void Snd19() { g_soundIdx = 19; UpdateSoundLabel(); PlayIdx(19); }
static void Snd20() { g_soundIdx = 20; UpdateSoundLabel(); PlayIdx(20); }
static void Snd21() { g_soundIdx = 21; UpdateSoundLabel(); PlayIdx(21); }
static void Snd22() { g_soundIdx = 22; UpdateSoundLabel(); PlayIdx(22); }

// ---- generated item shapes (92) ----
// Same rules as every other shape: the item in your left hand, through QueueItems
// (one batch every 3 s, at most 27 items, 2 a frame), always starting 0.7 m or more in front of you.
struct ShapeDef { int kind, a, b, c; };
static const ShapeDef kShapeDefs[] = {
    {0, 3, 0, 0},   // Ring 3
    {0, 4, 0, 0},   // Ring 4
    {0, 5, 0, 0},   // Ring 5
    {0, 6, 0, 0},   // Ring 6
    {0, 7, 0, 0},   // Ring 7
    {0, 8, 0, 0},   // Ring 8
    {0, 9, 0, 0},   // Ring 9
    {0, 10, 0, 0},   // Ring 10
    {0, 11, 0, 0},   // Ring 11
    {0, 12, 0, 0},   // Ring 12
    {0, 13, 0, 0},   // Ring 13
    {0, 14, 0, 0},   // Ring 14
    {0, 15, 0, 0},   // Ring 15
    {0, 16, 0, 0},   // Ring 16
    {1, 4, 0, 0},   // Star 4 Point
    {1, 5, 0, 0},   // Star 5 Point
    {1, 6, 0, 0},   // Star 6 Point
    {1, 7, 0, 0},   // Star 7 Point
    {1, 8, 0, 0},   // Star 8 Point
    {1, 9, 0, 0},   // Star 9 Point
    {1, 10, 0, 0},   // Star 10 Point
    {1, 11, 0, 0},   // Star 11 Point
    {1, 12, 0, 0},   // Star 12 Point
    {2, 3, 0, 0},   // Row 3
    {2, 4, 0, 0},   // Row 4
    {2, 5, 0, 0},   // Row 5
    {2, 6, 0, 0},   // Row 6
    {2, 7, 0, 0},   // Row 7
    {2, 8, 0, 0},   // Row 8
    {2, 9, 0, 0},   // Row 9
    {2, 10, 0, 0},   // Row 10
    {2, 11, 0, 0},   // Row 11
    {2, 12, 0, 0},   // Row 12
    {3, 2, 3, 0},   // Grid 2x3
    {3, 2, 4, 0},   // Grid 2x4
    {3, 2, 5, 0},   // Grid 2x5
    {3, 2, 6, 0},   // Grid 2x6
    {3, 3, 4, 0},   // Grid 3x4
    {3, 3, 5, 0},   // Grid 3x5
    {3, 3, 6, 0},   // Grid 3x6
    {3, 4, 5, 0},   // Grid 4x5
    {3, 4, 6, 0},   // Grid 4x6
    {4, 2, 0, 0},   // Spiral S
    {4, 3, 0, 0},   // Spiral M
    {4, 4, 0, 0},   // Spiral L
    {5, 2, 2, 2},   // Box 2x2x2
    {5, 3, 2, 3},   // Box 3x2x3
    {5, 2, 3, 2},   // Box 2x3x2
    {5, 3, 3, 2},   // Box 3x3x2
    {5, 2, 2, 4},   // Box 2x2x4
    {5, 4, 2, 3},   // Box 4x2x3
    {5, 3, 2, 4},   // Box 3x2x4
    {5, 2, 4, 3},   // Box 2x4x3
    {6, 3, 2, 0},   // Wall 3x2
    {6, 4, 2, 0},   // Wall 4x2
    {6, 5, 2, 0},   // Wall 5x2
    {6, 6, 2, 0},   // Wall 6x2
    {6, 3, 3, 0},   // Wall 3x3
    {6, 4, 3, 0},   // Wall 4x3
    {6, 6, 3, 0},   // Wall 6x3
    {6, 4, 4, 0},   // Wall 4x4
    {6, 5, 4, 0},   // Wall 5x4
    {6, 6, 4, 0},   // Wall 6x4
    {7, 2, 0, 0},   // Tower 2
    {7, 3, 0, 0},   // Tower 3
    {7, 4, 0, 0},   // Tower 4
    {7, 5, 0, 0},   // Tower 5
    {7, 6, 0, 0},   // Tower 6
    {7, 7, 0, 0},   // Tower 7
    {7, 8, 0, 0},   // Tower 8
    {7, 9, 0, 0},   // Tower 9
    {7, 10, 0, 0},   // Tower 10
    {7, 11, 0, 0},   // Tower 11
    {7, 12, 0, 0},   // Tower 12
    {8, 6, 0, 0},   // Zigzag 6
    {8, 8, 0, 0},   // Zigzag 8
    {8, 10, 0, 0},   // Zigzag 10
    {8, 12, 0, 0},   // Zigzag 12
    {8, 16, 0, 0},   // Zigzag 16
    {8, 20, 0, 0},   // Zigzag 20
    {9, 2, 0, 0},   // Plus 2
    {9, 3, 0, 0},   // Plus 3
    {9, 4, 0, 0},   // Plus 4
    {9, 5, 0, 0},   // Plus 5
    {9, 6, 0, 0},   // Plus 6
    {10, 12, 0, 0},   // Heart 12
    {10, 20, 0, 0},   // Heart 20
    {10, 24, 0, 0},   // Heart 24
    {11, 10, 0, 0},   // Helix 10
    {11, 14, 0, 0},   // Helix 14
    {11, 18, 0, 0},   // Helix 18
    {11, 22, 0, 0},   // Helix 22
};
static void SpawnDef(int id) {
    Il2CppObject *head = g_head.get();
    if (!head) return;
    const ShapeDef &d = kShapeDefs[id];
    struct P { float x, y, z; };
    std::vector<P> pts;
    switch (d.kind) {
    case 0: {                                          // a ring of a items
        float rad = fmaxf(0.35f, d.a * 0.09f);
        for (int i = 0; i < d.a; i++) { float an = i * (6.2831853f / d.a); pts.push_back({cosf(an) * rad, 0, sinf(an) * rad}); }
        break;
    }
    case 1:                                            // a star with a points
        for (int i = 0; i < d.a * 2; i++) {
            float an = i * (3.14159265f / d.a), rad = (i % 2 == 0) ? 0.8f : 0.35f;
            pts.push_back({sinf(an) * rad, 0, cosf(an) * rad});
        }
        break;
    case 2:                                            // a row of a
        for (int i = 0; i < d.a; i++) pts.push_back({(i - (d.a - 1) * 0.5f) * 0.4f, 0, 0});
        break;
    case 3:                                            // a flat grid, a x b
        for (int j = 0; j < d.b; j++)
            for (int i = 0; i < d.a; i++)
                pts.push_back({(i - (d.a - 1) * 0.5f) * 0.45f, 0, (j - (d.b - 1) * 0.5f) * 0.45f});
        break;
    case 4:                                            // a spiral with a loops
        for (int i = 0; i < d.a * 6; i++) { float an = i * (6.2831853f / 6.0f), rad = 0.2f + i * 0.04f; pts.push_back({cosf(an) * rad, 0, sinf(an) * rad}); }
        break;
    case 5:                                            // a box, a wide, b high, c deep
        for (int k = 0; k < d.b; k++)
            for (int j = 0; j < d.c; j++)
                for (int i = 0; i < d.a; i++)
                    pts.push_back({(i - (d.a - 1) * 0.5f) * 0.45f, k * 0.45f, (j - (d.c - 1) * 0.5f) * 0.45f});
        break;
    case 6:                                            // a wall, a across, b high
        for (int k = 0; k < d.b; k++)
            for (int i = 0; i < d.a; i++)
                pts.push_back({(i - (d.a - 1) * 0.5f) * 0.45f, k * 0.45f, 0});
        break;
    case 7:                                            // a tower of a
        for (int i = 0; i < d.a; i++) pts.push_back({0, i * 0.45f, 0});
        break;
    case 8:                                            // a zigzag of a
        for (int i = 0; i < d.a; i++) pts.push_back({(i - (d.a - 1) * 0.5f) * 0.3f, 0, (i % 2) * 0.35f});
        break;
    case 9:                                            // a plus sign with arms of a
        pts.push_back({0, 0, 0});
        for (int i = 1; i <= d.a; i++) {
            pts.push_back({i * 0.45f, 0, 0}); pts.push_back({-i * 0.45f, 0, 0});
            pts.push_back({0, 0, i * 0.45f}); pts.push_back({0, 0, -i * 0.45f});
        }
        break;
    case 10:                                           // a heart of a items
        for (int i = 0; i < d.a; i++) {
            float an = i * (6.2831853f / d.a);
            pts.push_back({16.0f * powf(sinf(an), 3.0f) * 0.04f, 0,
                           (13.0f * cosf(an) - 5.0f * cosf(2 * an) - 2.0f * cosf(3 * an) - cosf(4 * an)) * 0.04f});
        }
        break;
    default:                                           // a helix of a items
        for (int i = 0; i < d.a; i++) { float an = i * 0.8f; pts.push_back({cosf(an) * 0.4f, i * 0.25f, sinf(an) * 0.4f}); }
        break;
    }
    if (pts.size() > 27) pts.resize(27);
    float minZ = 1e9f;
    for (auto &q : pts) minZ = fminf(minZ, q.z);
    Vector3 f = Flat(G.trGetForward[head]()), r = Cross(Vector3(0, 1, 0), f);
    Vector3 base = G.trGetPosition[head]() + f * (0.7f - minZ) + Vector3(0, -0.4f, 0);
    std::vector<PendingItem> list;
    for (auto &q : pts) list.push_back({base + r * q.x + f * q.z + Vector3(0, q.y, 0), Vector3(0, 0, 0)});
    QueueItems(list);
}

// ---- generated buttons: levels, themes, sizes and shapes ----
static void PoseSpd0() { g_danceSpeed = 0.50f; Notify("Pose speed 0.5x"); }
static void PoseSpd1() { g_danceSpeed = 0.60f; Notify("Pose speed 0.6x"); }
static void PoseSpd2() { g_danceSpeed = 0.70f; Notify("Pose speed 0.7x"); }
static void PoseSpd3() { g_danceSpeed = 0.80f; Notify("Pose speed 0.8x"); }
static void PoseSpd4() { g_danceSpeed = 0.90f; Notify("Pose speed 0.9x"); }
static void PoseSpd5() { g_danceSpeed = 1.00f; Notify("Pose speed 1x"); }
static void PoseSpd6() { g_danceSpeed = 1.25f; Notify("Pose speed 1.2x"); }
static void PoseSpd7() { g_danceSpeed = 1.50f; Notify("Pose speed 1.5x"); }
static void PoseSpd8() { g_danceSpeed = 1.75f; Notify("Pose speed 1.8x"); }
static void PoseSpd9() { g_danceSpeed = 2.00f; Notify("Pose speed 2x"); }
static void ArmS0() { if (LeftHandItem()) SpawnDef(0); }
static void ArmS1() { if (LeftHandItem()) SpawnDef(1); }
static void ArmS2() { if (LeftHandItem()) SpawnDef(2); }
static void ArmS3() { if (LeftHandItem()) SpawnDef(3); }
static void ArmS4() { if (LeftHandItem()) SpawnDef(4); }
static void ArmS5() { if (LeftHandItem()) SpawnDef(5); }
static void ArmS6() { if (LeftHandItem()) SpawnDef(6); }
static void ArmS7() { if (LeftHandItem()) SpawnDef(7); }
static void ArmS8() { if (LeftHandItem()) SpawnDef(8); }
static void ArmS9() { if (LeftHandItem()) SpawnDef(9); }
static void ArmS10() { if (LeftHandItem()) SpawnDef(10); }
static void ArmS11() { if (LeftHandItem()) SpawnDef(11); }
static void ArmS12() { if (LeftHandItem()) SpawnDef(12); }
static void ArmS13() { if (LeftHandItem()) SpawnDef(13); }
static void ArmS14() { if (LeftHandItem()) SpawnDef(14); }
static void ArmS15() { if (LeftHandItem()) SpawnDef(15); }
static void ArmS16() { if (LeftHandItem()) SpawnDef(16); }
static void ArmS17() { if (LeftHandItem()) SpawnDef(17); }
static void ArmS18() { if (LeftHandItem()) SpawnDef(18); }
static void ArmS19() { if (LeftHandItem()) SpawnDef(19); }
static void ArmS20() { if (LeftHandItem()) SpawnDef(20); }
static void ArmS21() { if (LeftHandItem()) SpawnDef(21); }
static void ArmS22() { if (LeftHandItem()) SpawnDef(22); }
static void ArmS23() { if (LeftHandItem()) SpawnDef(23); }
static void ArmS24() { if (LeftHandItem()) SpawnDef(24); }
static void ArmS25() { if (LeftHandItem()) SpawnDef(25); }
static void ArmS26() { if (LeftHandItem()) SpawnDef(26); }
static void ArmS27() { if (LeftHandItem()) SpawnDef(27); }
static void ArmS28() { if (LeftHandItem()) SpawnDef(28); }
static void ArmS29() { if (LeftHandItem()) SpawnDef(29); }
static void ArmS30() { if (LeftHandItem()) SpawnDef(30); }
static void ArmS31() { if (LeftHandItem()) SpawnDef(31); }
static void ArmS32() { if (LeftHandItem()) SpawnDef(32); }
static void ArmS33() { if (LeftHandItem()) SpawnDef(33); }
static void ArmS34() { if (LeftHandItem()) SpawnDef(34); }
static void ArmS35() { if (LeftHandItem()) SpawnDef(35); }
static void ArmS36() { if (LeftHandItem()) SpawnDef(36); }
static void ArmS37() { if (LeftHandItem()) SpawnDef(37); }
static void ArmS38() { if (LeftHandItem()) SpawnDef(38); }
static void ArmS39() { if (LeftHandItem()) SpawnDef(39); }
static void ArmS40() { if (LeftHandItem()) SpawnDef(40); }
static void ArmS41() { if (LeftHandItem()) SpawnDef(41); }
static void ArmS42() { if (LeftHandItem()) SpawnDef(42); }
static void ArmS43() { if (LeftHandItem()) SpawnDef(43); }
static void ArmS44() { if (LeftHandItem()) SpawnDef(44); }
static void ArmS45() { if (LeftHandItem()) SpawnDef(45); }
static void ArmS46() { if (LeftHandItem()) SpawnDef(46); }
static void ArmS47() { if (LeftHandItem()) SpawnDef(47); }
static void ArmS48() { if (LeftHandItem()) SpawnDef(48); }
static void ArmS49() { if (LeftHandItem()) SpawnDef(49); }
static void ArmS50() { if (LeftHandItem()) SpawnDef(50); }
static void ArmS51() { if (LeftHandItem()) SpawnDef(51); }
static void ArmS52() { if (LeftHandItem()) SpawnDef(52); }
static void ArmS53() { if (LeftHandItem()) SpawnDef(53); }
static void ArmS54() { if (LeftHandItem()) SpawnDef(54); }
static void ArmS55() { if (LeftHandItem()) SpawnDef(55); }
static void ArmS56() { if (LeftHandItem()) SpawnDef(56); }
static void ArmS57() { if (LeftHandItem()) SpawnDef(57); }
static void ArmS58() { if (LeftHandItem()) SpawnDef(58); }
static void ArmS59() { if (LeftHandItem()) SpawnDef(59); }
static void ArmS60() { if (LeftHandItem()) SpawnDef(60); }
static void ArmS61() { if (LeftHandItem()) SpawnDef(61); }
static void ArmS62() { if (LeftHandItem()) SpawnDef(62); }
static void ArmS63() { if (LeftHandItem()) SpawnDef(63); }
static void ArmS64() { if (LeftHandItem()) SpawnDef(64); }
static void ArmS65() { if (LeftHandItem()) SpawnDef(65); }
static void ArmS66() { if (LeftHandItem()) SpawnDef(66); }
static void ArmS67() { if (LeftHandItem()) SpawnDef(67); }
static void ArmS68() { if (LeftHandItem()) SpawnDef(68); }
static void ArmS69() { if (LeftHandItem()) SpawnDef(69); }
static void ArmS70() { if (LeftHandItem()) SpawnDef(70); }
static void ArmS71() { if (LeftHandItem()) SpawnDef(71); }
static void ArmS72() { if (LeftHandItem()) SpawnDef(72); }
static void ArmS73() { if (LeftHandItem()) SpawnDef(73); }
static void ArmS74() { if (LeftHandItem()) SpawnDef(74); }
static void ArmS75() { if (LeftHandItem()) SpawnDef(75); }
static void ArmS76() { if (LeftHandItem()) SpawnDef(76); }
static void ArmS77() { if (LeftHandItem()) SpawnDef(77); }
static void ArmS78() { if (LeftHandItem()) SpawnDef(78); }
static void ArmS79() { if (LeftHandItem()) SpawnDef(79); }
static void ArmS80() { if (LeftHandItem()) SpawnDef(80); }
static void ArmS81() { if (LeftHandItem()) SpawnDef(81); }
static void ArmS82() { if (LeftHandItem()) SpawnDef(82); }
static void ArmS83() { if (LeftHandItem()) SpawnDef(83); }
static void ArmS84() { if (LeftHandItem()) SpawnDef(84); }
static void ArmS85() { if (LeftHandItem()) SpawnDef(85); }
static void ArmS86() { if (LeftHandItem()) SpawnDef(86); }
static void ArmS87() { if (LeftHandItem()) SpawnDef(87); }
static void ArmS88() { if (LeftHandItem()) SpawnDef(88); }
static void ArmS89() { if (LeftHandItem()) SpawnDef(89); }
static void ArmS90() { if (LeftHandItem()) SpawnDef(90); }
static void ArmS91() { if (LeftHandItem()) SpawnDef(91); }
static void FlyNormal() { g_flyLevel = 0; Notify("Fly speed: normal"); }
static void JumpNormal() { g_jumpLevel = 0; ApplySpeedBoost(); Notify("Jump: normal"); }
static void FlyLvl1() { g_flyLevel = 1; Notify("Fly speed 1"); }
static void FlyLvl2() { g_flyLevel = 2; Notify("Fly speed 2"); }
static void FlyLvl3() { g_flyLevel = 3; Notify("Fly speed 3"); }
static void FlyLvl4() { g_flyLevel = 4; Notify("Fly speed 4"); }
static void FlyLvl5() { g_flyLevel = 5; Notify("Fly speed 5"); }
static void FlyLvl6() { g_flyLevel = 6; Notify("Fly speed 6"); }
static void FlyLvl7() { g_flyLevel = 7; Notify("Fly speed 7"); }
static void FlyLvl8() { g_flyLevel = 8; Notify("Fly speed 8"); }
static void FlyLvl9() { g_flyLevel = 9; Notify("Fly speed 9"); }
static void FlyLvl10() { g_flyLevel = 10; Notify("Fly speed 10"); }
static void FlyLvl11() { g_flyLevel = 11; Notify("Fly speed 11"); }
static void FlyLvl12() { g_flyLevel = 12; Notify("Fly speed 12"); }
static void FlyLvl13() { g_flyLevel = 13; Notify("Fly speed 13"); }
static void FlyLvl14() { g_flyLevel = 14; Notify("Fly speed 14"); }
static void FlyLvl15() { g_flyLevel = 15; Notify("Fly speed 15"); }
static void FlyLvl16() { g_flyLevel = 16; Notify("Fly speed 16"); }
static void FlyLvl17() { g_flyLevel = 17; Notify("Fly speed 17"); }
static void FlyLvl18() { g_flyLevel = 18; Notify("Fly speed 18"); }
static void FlyLvl19() { g_flyLevel = 19; Notify("Fly speed 19"); }
static void FlyLvl20() { g_flyLevel = 20; Notify("Fly speed 20"); }
static void JumpLvl1() { g_jumpLevel = 1; ApplySpeedBoost(); Notify("Jump level 1"); }
static void JumpLvl2() { g_jumpLevel = 2; ApplySpeedBoost(); Notify("Jump level 2"); }
static void JumpLvl3() { g_jumpLevel = 3; ApplySpeedBoost(); Notify("Jump level 3"); }
static void JumpLvl4() { g_jumpLevel = 4; ApplySpeedBoost(); Notify("Jump level 4"); }
static void JumpLvl5() { g_jumpLevel = 5; ApplySpeedBoost(); Notify("Jump level 5"); }
static void JumpLvl6() { g_jumpLevel = 6; ApplySpeedBoost(); Notify("Jump level 6"); }
static void JumpLvl7() { g_jumpLevel = 7; ApplySpeedBoost(); Notify("Jump level 7"); }
static void JumpLvl8() { g_jumpLevel = 8; ApplySpeedBoost(); Notify("Jump level 8"); }
static void JumpLvl9() { g_jumpLevel = 9; ApplySpeedBoost(); Notify("Jump level 9"); }
static void JumpLvl10() { g_jumpLevel = 10; ApplySpeedBoost(); Notify("Jump level 10"); }
static void JumpLvl11() { g_jumpLevel = 11; ApplySpeedBoost(); Notify("Jump level 11"); }
static void JumpLvl12() { g_jumpLevel = 12; ApplySpeedBoost(); Notify("Jump level 12"); }
static void JumpLvl13() { g_jumpLevel = 13; ApplySpeedBoost(); Notify("Jump level 13"); }
static void JumpLvl14() { g_jumpLevel = 14; ApplySpeedBoost(); Notify("Jump level 14"); }
static void JumpLvl15() { g_jumpLevel = 15; ApplySpeedBoost(); Notify("Jump level 15"); }
static void JumpLvl16() { g_jumpLevel = 16; ApplySpeedBoost(); Notify("Jump level 16"); }
static void JumpLvl17() { g_jumpLevel = 17; ApplySpeedBoost(); Notify("Jump level 17"); }
static void JumpLvl18() { g_jumpLevel = 18; ApplySpeedBoost(); Notify("Jump level 18"); }
static void JumpLvl19() { g_jumpLevel = 19; ApplySpeedBoost(); Notify("Jump level 19"); }
static void JumpLvl20() { g_jumpLevel = 20; ApplySpeedBoost(); Notify("Jump level 20"); }
static void DashLvl1() { g_dashPower = 7.0f; f_dash = true; Notify("Dash power 1"); }
static void DashLvl2() { g_dashPower = 10.0f; f_dash = true; Notify("Dash power 2"); }
static void DashLvl3() { g_dashPower = 13.0f; f_dash = true; Notify("Dash power 3"); }
static void DashLvl4() { g_dashPower = 16.0f; f_dash = true; Notify("Dash power 4"); }
static void DashLvl5() { g_dashPower = 19.0f; f_dash = true; Notify("Dash power 5"); }
static void DashLvl6() { g_dashPower = 22.0f; f_dash = true; Notify("Dash power 6"); }
static void DashLvl7() { g_dashPower = 25.0f; f_dash = true; Notify("Dash power 7"); }
static void DashLvl8() { g_dashPower = 28.0f; f_dash = true; Notify("Dash power 8"); }
static void DashLvl9() { g_dashPower = 31.0f; f_dash = true; Notify("Dash power 9"); }
static void DashLvl10() { g_dashPower = 34.0f; f_dash = true; Notify("Dash power 10"); }
static void GravNormal() { f_lowGravity = false; g_gravCancel = 0.7f; Notify("Gravity: normal"); }
static void GravLvl0() { g_gravCancel = 1.00f; f_lowGravity = true; Notify("Gravity 0%%"); }
static void GravLvl10() { g_gravCancel = 0.90f; f_lowGravity = true; Notify("Gravity 10%%"); }
static void GravLvl20() { g_gravCancel = 0.80f; f_lowGravity = true; Notify("Gravity 20%%"); }
static void GravLvl30() { g_gravCancel = 0.70f; f_lowGravity = true; Notify("Gravity 30%%"); }
static void GravLvl40() { g_gravCancel = 0.60f; f_lowGravity = true; Notify("Gravity 40%%"); }
static void GravLvl50() { g_gravCancel = 0.50f; f_lowGravity = true; Notify("Gravity 50%%"); }
static void GravLvl60() { g_gravCancel = 0.40f; f_lowGravity = true; Notify("Gravity 60%%"); }
static void GravLvl70() { g_gravCancel = 0.30f; f_lowGravity = true; Notify("Gravity 70%%"); }
static void GravLvl80() { g_gravCancel = 0.20f; f_lowGravity = true; Notify("Gravity 80%%"); }
static void GravLvl90() { g_gravCancel = 0.10f; f_lowGravity = true; Notify("Gravity 90%%"); }
static void ThemeSet0() { g_theme = 12; g_dirty = true; Notify("Theme: Ruby"); }
static void ThemeSet1() { g_theme = 13; g_dirty = true; Notify("Theme: Scarlet"); }
static void ThemeSet2() { g_theme = 14; g_dirty = true; Notify("Theme: Coral"); }
static void ThemeSet3() { g_theme = 15; g_dirty = true; Notify("Theme: Ember"); }
static void ThemeSet4() { g_theme = 16; g_dirty = true; Notify("Theme: Amber"); }
static void ThemeSet5() { g_theme = 17; g_dirty = true; Notify("Theme: Honey"); }
static void ThemeSet6() { g_theme = 18; g_dirty = true; Notify("Theme: Lemon"); }
static void ThemeSet7() { g_theme = 19; g_dirty = true; Notify("Theme: Lime"); }
static void ThemeSet8() { g_theme = 20; g_dirty = true; Notify("Theme: Fern"); }
static void ThemeSet9() { g_theme = 21; g_dirty = true; Notify("Theme: Jade"); }
static void ThemeSet10() { g_theme = 22; g_dirty = true; Notify("Theme: Emerald"); }
static void ThemeSet11() { g_theme = 23; g_dirty = true; Notify("Theme: Spring"); }
static void ThemeSet12() { g_theme = 24; g_dirty = true; Notify("Theme: Teal"); }
static void ThemeSet13() { g_theme = 25; g_dirty = true; Notify("Theme: Aqua"); }
static void ThemeSet14() { g_theme = 26; g_dirty = true; Notify("Theme: Azure"); }
static void ThemeSet15() { g_theme = 27; g_dirty = true; Notify("Theme: Sky"); }
static void ThemeSet16() { g_theme = 28; g_dirty = true; Notify("Theme: Cobalt"); }
static void ThemeSet17() { g_theme = 29; g_dirty = true; Notify("Theme: Sapphire"); }
static void ThemeSet18() { g_theme = 30; g_dirty = true; Notify("Theme: Indigo"); }
static void ThemeSet19() { g_theme = 31; g_dirty = true; Notify("Theme: Violet"); }
static void ThemeSet20() { g_theme = 32; g_dirty = true; Notify("Theme: Grape"); }
static void ThemeSet21() { g_theme = 33; g_dirty = true; Notify("Theme: Orchid"); }
static void ThemeSet22() { g_theme = 34; g_dirty = true; Notify("Theme: Magenta"); }
static void ThemeSet23() { g_theme = 35; g_dirty = true; Notify("Theme: Fuchsia"); }
static void ThemeSet24() { g_theme = 36; g_dirty = true; Notify("Theme: Rose"); }
static void ThemeSet25() { g_theme = 37; g_dirty = true; Notify("Theme: Blush"); }
static void ThemeSet26() { g_theme = 38; g_dirty = true; Notify("Theme: Cherry"); }
static void ThemeSet27() { g_theme = 39; g_dirty = true; Notify("Theme: Cranberry"); }
static void ThemeSet28() { g_theme = 40; g_dirty = true; Notify("Theme: Peach"); }
static void ThemeSet29() { g_theme = 41; g_dirty = true; Notify("Theme: Mango"); }
static void ThemeSet30() { g_theme = 42; g_dirty = true; Notify("Theme: Moss"); }
static void ThemeSet31() { g_theme = 43; g_dirty = true; Notify("Theme: Pine"); }
static void ThemeSet32() { g_theme = 44; g_dirty = true; Notify("Theme: Ocean"); }
static void ThemeSet33() { g_theme = 45; g_dirty = true; Notify("Theme: Ice"); }
static void ThemeSet34() { g_theme = 46; g_dirty = true; Notify("Theme: Lavender"); }
static void ThemeSet35() { g_theme = 47; g_dirty = true; Notify("Theme: Plum"); }
static void SizeSet0() { SetMenuSize(0.50f); }
static void SizeSet1() { SetMenuSize(0.60f); }
static void SizeSet2() { SetMenuSize(0.70f); }
static void SizeSet3() { SetMenuSize(0.80f); }
static void SizeSet4() { SetMenuSize(0.90f); }
static void SizeSet5() { SetMenuSize(1.10f); }
static void SizeSet6() { SetMenuSize(1.25f); }
static void SizeSet7() { SetMenuSize(1.50f); }
static void SizeSet8() { SetMenuSize(1.75f); }
static void SizeSet9() { SetMenuSize(2.00f); }

// Spawn On Hand: each right-trigger press (grip open) spawns the item at your hand.
static bool g_onHandTrigger = false;
static void SpawnOnHandTick(const Buttons &in) {
    bool press = in.rtrig && !g_onHandTrigger;
    g_onHandTrigger = in.rtrig;
    Il2CppObject *hand = g_rightHand.get();
    if (!f_spawnOnHand || !press || in.rgrip || g_menuShown || !hand || g_time < g_nextItem) return;
    if (!LeftHandItem()) return;
    if (g_itemMult > 1) {                       // Item Multiplier: a small batch at your right hand
        Vector3 hp = G.trGetPosition[hand]() + G.trGetForward[hand]() * 0.1f;
        std::vector<PendingItem> list;
        for (int i = 0; i < g_itemMult; i++) {
            float a = i * 2.4f, rad = 0.06f + i * 0.015f;
            list.push_back({hp + Vector3(cosf(a) * rad, (i % 3) * 0.1f, sinf(a) * rad), Vector3(0, 0, 0)});
        }
        QueueItems(list);                       // one batch every 3 s and at most 27, like the shapes
        return;
    }
    g_nextItem = g_time + 0.3;
    g_itemQueue.push_back({G.trGetPosition[hand]() + G.trGetForward[hand]() * 0.1f, Vector3(0, 0, 0)});
}

// ---- callbacks ---------------------------------------------------------------

static void OnRoomChange() {
    ClearDecor();
    VoiceStop();
    VC.rec.clear();
    SoundShutdown();
    // The old room's objects are gone (or about to be): forget them, build again.
    for (int r = 0; r < kRows; r++) { g_rowCube[r].clear(); g_rowLabel[r].clear(); g_rowText[r].clear(); }
    for (Ref *r : {&g_board, &g_bg, &g_outline, &g_title, &g_titleBar, &g_prev, &g_next, &g_notifyText, &g_tipText}) r->clear();
    for (Ref *r : {&g_menu, &g_pointer, &g_notifyGo, &g_leftPlatform, &g_rightPlatform, &g_gunBall, &g_gunLineGo}) Kill(*r);
    for (int i = 0; i < 6; i++) { g_bgEx[i].clear(); g_outEx[i].clear(); }
    PauseTrackers(false);
    g_dancing = false;
    g_gunLine.clear();
    if (g_noclipOn && g_player.get()) {           // leaving the room mid-noclip: put things back
        Buttons none;                             // trigger "released"
        NoclipTick(none);
    }
    g_boosted = nullptr;
    g_gravityOff = false;
    for (Ref *r : {&g_player, &g_rigidbody, &g_leftHand, &g_rightHand, &g_head}) r->clear();
    g_built = false; g_menuShown = false; g_touching = -2; g_haveSpot = false;
}

static void OnUpdate(float dt) {
    g_time += dt;
    g_frames++;
    static double fpsStart = 0;
    if (g_time - fpsStart >= 1.0) { g_fps = (float)(g_frames / (g_time - fpsStart)); g_frames = 0; fpsStart = g_time; }
    if (!Lookups()) return;
    auto *player = (Il2CppObject *)ym::api()->localPlayer();
    if (!player) return;
    if (player != g_player.get()) {                 // first frame in this room
        OnRoomChange();
        g_player.set(player);
        g_leftHand.set(G.leftHand[player]());
        g_rightHand.set(G.rightHand[player]());
        g_head.set(G.headTransform[player]());
        Il2CppObject *pgo = G.compGameObject[player]();
        if (pgo) g_rigidbody.set(G.goGetComponent[pgo](G.tRigidbody));
    }
    if (!g_leftHand.get() || !g_rightHand.get() || !g_head.get()) return;
    if (!g_built) BuildMenu();

    Buttons in = ReadButtons();
    ShowMenu(f_rightHand ? in.b : in.y);
    if (g_menuShown) {
        PlaceMenu();
        CheckPress();
        if (g_dirty) RenderPage();
        UpdateTitle();
        AnimateMenu();
    }
    if (g_notifyUntil > 0 && g_time >= g_notifyUntil) { Show(g_notifyGo, false); g_notifyUntil = 0; }
    FlyTick(in, dt);
    LowGravityTick(in, dt);
    MoreMovementTick(in, dt);
    PlatformFor(in.lgrip, g_leftPlatform, g_leftHand);
    PlatformFor(in.rgrip, g_rightPlatform, g_rightHand);
    VelocityFlyTick(in);
    CarTick(in);
    IronManTick(in, dt);
    ApplySpeedBoost();
    ApplyGravity();
    NoclipTick(in);
    TeleportGunTick(in);
    try { SpawnOnHandTick(in); ItemQueueTick(); }
    catch (...) { g_itemQueue.clear(); ym::log("starter-menu: item code error"); }
    try { VoiceTick(); } catch (...) {}
    DanceTick(in, dt);              // last: it moves your hands after everything else
}

YM_EXPORT int YeepsMod_Init(const YeepsModApi *api) {
    if (!ym::init(api)) return -1;
    ym::onUpdate(OnUpdate);
    ym::log("t3wtr4fvr loaded on %s (%s)", api->gameVersion, api->buildId);
    return 0;
}
