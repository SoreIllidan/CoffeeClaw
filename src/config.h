// Coffeeclaw - offsets.ini contract
#pragma once

#include "common.h"
#include <cwchar>
#include <cstdlib>

namespace claw {

struct Config {
    std::wstring processName = L"ArkAscended.exe";

    // --- resolution of the two roots -------------------------------------
    // Absolute address (in ArkAscended.exe) of the pointer that holds GWorld.
    uintptr_t gworldSlot = 0;
    // Optional IDA-style pattern that locates GWorldSlot (mov reg,[rip+disp]).
    std::string gworldPattern;
    int gworldDispOffset = 3;
    int gworldMatchLen = 7;

    // Absolute address of the FName pool (TNameEntryArray / FNamePool::Blocks).
    uintptr_t gnamesSlot = 0;
    std::string gnamesPattern;
    int gnamesDispOffset = 3;
    int gnamesMatchLen = 7;
    // 0 => slot holds the pool address directly, 1 => slot is a pointer to the pool
    int gnamesIndirection = 1;

    // --- UE object graph -------------------------------------------------
    uintptr_t offClassPrivate        = 0x10; // UObject::ClassPrivate
    uintptr_t offNamePrivate         = 0x18; // UObject::NamePrivate (FName)
    uintptr_t offSuperStruct         = 0x50; // UStruct::SuperStruct
    uintptr_t offPersistentLevel     = 0x300;
    uintptr_t offOwningGameInstance  = 0x4A8;
    uintptr_t offLocalPlayers        = 0x38; // UGameInstance::LocalPlayers (TArray)
    uintptr_t offLocalPlayerCtrl     = 0x30; // ULocalPlayer::PlayerController
    uintptr_t offCameraManager       = 0x560; // APlayerController::PlayerCameraManager
uintptr_t offLocalPawn           = 0x548; // APlayerController::Pawn
uintptr_t offMovementComp        = 0x550; // ACharacter::CharacterMovement
uintptr_t offMovePos1            = 0x380; // movimiento: posicion interna
uintptr_t offMovePos2            = 0x4E8;
uintptr_t offMovePos3            = 0x1138;
    uintptr_t offCameraCache         = 0x540; // APlayerCameraManager::CameraCachePrivate
    uintptr_t offLevelActors         = 0xA0;  // ULevel::Actors (TArray)
    uintptr_t offActorRootComponent  = 0x120; // AActor::RootComponent
    uintptr_t offComponentToWorld    = 0x120; // USceneComponent::ComponentToWorld
    uintptr_t offTransformTranslation = 0x0;
uintptr_t offRelativeLocation     = 0x230; // USceneComponent::RelativeLocation
    uintptr_t offMeshComponent        = 0x548;
    uintptr_t offMeshCtw              = 0x230;
    uintptr_t offMeshBounds           = 0x138;
    uintptr_t offMeshBones            = 0x9E0;
    uintptr_t offMeshAsset            = 0x660;
    uintptr_t offBoneInfo             = 0x380;
    uintptr_t offRefPose              = 0x4E8;
    uintptr_t refPoseStride           = 0x28;
    uintptr_t offAnimBones            = 0x6D8;
    uintptr_t animStride              = 0x60; // FTransform::Translation
    uintptr_t offFov                 = 0x30;  // FMinimalViewInfo::FOV (inside camera cache)

    // --- filter ----------------------------------------------------------
    std::wstring dinoClassHint = L"PrimalDinoCharacter";
    int maxActors = 20000;
    double maxDistanceM = 400.0;

    // --- runtime ---------------------------------------------------------
    int readerHz = 25;
    int actorRefreshMs = 500;

    std::wstring iniPath;

    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path) const;
    std::wstring FirstMissing() const;
};

namespace detail {
inline std::wstring GetS(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[1024]{};
    ::GetPrivateProfileStringW(sec, key, def, buf, 1024, ini.c_str());
    return buf;
}
inline uintptr_t GetHex(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, uintptr_t def) {
    std::wstring v = GetS(ini, sec, key, L"");
    if (v.empty()) return def;
    wchar_t* end = nullptr;
    unsigned long long parsed = wcstoull(v.c_str(), &end, 16);
    if (end == v.c_str()) return def;
    return (uintptr_t)parsed;
}
inline std::string GetA(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, const char* def) {
    wchar_t buf[1024]{};
    std::wstring d;
    if (def) { int n = ::MultiByteToWideChar(CP_UTF8, 0, def, -1, nullptr, 0); d.resize((size_t)n); ::MultiByteToWideChar(CP_UTF8, 0, def, -1, &d[0], n); }
    ::GetPrivateProfileStringW(sec, key, d.c_str(), buf, 1024, ini.c_str());
    return Narrow(buf);
}
inline int GetInt(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, int def) {
    return (int)::GetPrivateProfileIntW(sec, key, def, ini.c_str());
}
inline double GetDouble(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, double def) {
    std::wstring v = GetS(ini, sec, key, L"");
    if (v.empty()) return def;
    return _wtof(v.c_str());
}
} // namespace detail

inline bool Config::Load(const std::wstring& path) {
    iniPath = path;
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Save(path);
        return false;
    }
    processName       = detail::GetS(path, L"process", L"Name", processName.c_str());
    gworldSlot        = detail::GetHex(path, L"roots", L"GWorldSlot", gworldSlot);
    gworldPattern     = detail::GetA(path, L"roots", L"GWorldPattern", gworldPattern.c_str());
    gworldDispOffset  = detail::GetInt(path, L"roots", L"GWorldDispOffset", gworldDispOffset);
    gworldMatchLen    = detail::GetInt(path, L"roots", L"GWorldMatchLen", gworldMatchLen);
    gnamesSlot        = detail::GetHex(path, L"roots", L"GNamesSlot", gnamesSlot);
    gnamesPattern     = detail::GetA(path, L"roots", L"GNamesPattern", gnamesPattern.c_str());
    gnamesDispOffset  = detail::GetInt(path, L"roots", L"GNamesDispOffset", gnamesDispOffset);
    gnamesMatchLen    = detail::GetInt(path, L"roots", L"GNamesMatchLen", gnamesMatchLen);
    gnamesIndirection = detail::GetInt(path, L"roots", L"GNamesIndirection", gnamesIndirection);

    offClassPrivate         = detail::GetHex(path, L"offsets", L"Object.ClassPrivate", offClassPrivate);
    offNamePrivate          = detail::GetHex(path, L"offsets", L"Object.NamePrivate", offNamePrivate);
    offSuperStruct          = detail::GetHex(path, L"offsets", L"Struct.SuperStruct", offSuperStruct);
    offPersistentLevel      = detail::GetHex(path, L"offsets", L"World.PersistentLevel", offPersistentLevel);
    offOwningGameInstance   = detail::GetHex(path, L"offsets", L"World.OwningGameInstance", offOwningGameInstance);
    offLocalPlayers         = detail::GetHex(path, L"offsets", L"GameInstance.LocalPlayers", offLocalPlayers);
    offLocalPlayerCtrl      = detail::GetHex(path, L"offsets", L"LocalPlayer.PlayerController", offLocalPlayerCtrl);
    offCameraManager        = detail::GetHex(path, L"offsets", L"PlayerController.CameraManager", offCameraManager);
    offLocalPawn            = detail::GetHex(path, L"offsets", L"PlayerController.Pawn", offLocalPawn);
    offMovementComp         = detail::GetHex(path, L"offsets", L"Character.Movement", offMovementComp);
    offMovePos1             = detail::GetHex(path, L"offsets", L"Movement.Pos1", offMovePos1);
    offMovePos2             = detail::GetHex(path, L"offsets", L"Movement.Pos2", offMovePos2);
    offMovePos3             = detail::GetHex(path, L"offsets", L"Movement.Pos3", offMovePos3);
    offCameraCache          = detail::GetHex(path, L"offsets", L"CameraManager.CameraCache", offCameraCache);
    offLevelActors          = detail::GetHex(path, L"offsets", L"Level.Actors", offLevelActors);
    offActorRootComponent   = detail::GetHex(path, L"offsets", L"Actor.RootComponent", offActorRootComponent);
    offComponentToWorld     = detail::GetHex(path, L"offsets", L"Scene.ComponentToWorld", offComponentToWorld);
    offTransformTranslation = detail::GetHex(path, L"offsets", L"Transform.Translation", offTransformTranslation);
    offRelativeLocation     = detail::GetHex(path, L"offsets", L"Scene.RelativeLocation", offRelativeLocation);
    offMeshComponent        = detail::GetHex(path, L"offsets", L"Mesh.Component", offMeshComponent);
    offMeshCtw              = detail::GetHex(path, L"offsets", L"Mesh.ComponentToWorld", offMeshCtw);
    offMeshBounds           = detail::GetHex(path, L"offsets", L"Mesh.BoxExtent", offMeshBounds);
    offMeshBones            = detail::GetHex(path, L"offsets", L"Mesh.Bones", offMeshBones);
    offMeshAsset            = detail::GetHex(path, L"offsets", L"Mesh.Asset", offMeshAsset);
    offBoneInfo             = detail::GetHex(path, L"offsets", L"Skeleton.BoneInfo", offBoneInfo);
    offRefPose              = detail::GetHex(path, L"offsets", L"Skeleton.RefPose", offRefPose);
    refPoseStride           = detail::GetHex(path, L"offsets", L"Skeleton.PoseStride", refPoseStride);
    offAnimBones            = detail::GetHex(path, L"offsets", L"Skeleton.AnimBones", offAnimBones);
    animStride              = detail::GetHex(path, L"offsets", L"Skeleton.AnimStride", animStride);
    offFov                  = detail::GetHex(path, L"offsets", L"Camera.FOV", offFov);

    dinoClassHint    = detail::GetS(path, L"filter", L"DinoClassHint", dinoClassHint.c_str());
    maxActors        = detail::GetInt(path, L"filter", L"MaxActors", maxActors);
    maxDistanceM     = detail::GetDouble(path, L"filter", L"MaxDistanceM", maxDistanceM);
    readerHz         = detail::GetInt(path, L"runtime", L"ReaderHz", readerHz);
    actorRefreshMs   = detail::GetInt(path, L"runtime", L"ActorRefreshMs", actorRefreshMs);
    if (readerHz < 1) readerHz = 1;
    if (readerHz > 240) readerHz = 240;
    if (maxActors < 16) maxActors = 16;
    return true;
}

inline bool Config::Save(const std::wstring& path) const {
    wchar_t num[64];
    auto W = [&](const wchar_t* sec, const wchar_t* key, const std::wstring& val) {
        ::WritePrivateProfileStringW(sec, key, val.c_str(), path.c_str());
    };
    auto Hs = [&](uintptr_t v) { std::swprintf(num, 64, L"0x%llX", (unsigned long long)v); return std::wstring(num); };
    auto Ds = [&](double v) { std::swprintf(num, 64, L"%.3f", v); return std::wstring(num); };
    auto Is = [&](int v) { std::swprintf(num, 64, L"%d", v); return std::wstring(num); };

    W(L"process", L"Name", processName);
    W(L"roots", L"GWorldSlot", Hs(gworldSlot));
    W(L"roots", L"GWorldPattern", std::wstring(gworldPattern.begin(), gworldPattern.end()));
    W(L"roots", L"GWorldDispOffset", Is(gworldDispOffset));
    W(L"roots", L"GWorldMatchLen", Is(gworldMatchLen));
    W(L"roots", L"GNamesSlot", Hs(gnamesSlot));
    W(L"roots", L"GNamesPattern", std::wstring(gnamesPattern.begin(), gnamesPattern.end()));
    W(L"roots", L"GNamesDispOffset", Is(gnamesDispOffset));
    W(L"roots", L"GNamesMatchLen", Is(gnamesMatchLen));
    W(L"roots", L"GNamesIndirection", Is(gnamesIndirection));

    W(L"offsets", L"Object.ClassPrivate", Hs(offClassPrivate));
    W(L"offsets", L"Object.NamePrivate", Hs(offNamePrivate));
    W(L"offsets", L"Struct.SuperStruct", Hs(offSuperStruct));
    W(L"offsets", L"World.PersistentLevel", Hs(offPersistentLevel));
    W(L"offsets", L"World.OwningGameInstance", Hs(offOwningGameInstance));
    W(L"offsets", L"GameInstance.LocalPlayers", Hs(offLocalPlayers));
    W(L"offsets", L"LocalPlayer.PlayerController", Hs(offLocalPlayerCtrl));
    W(L"offsets", L"PlayerController.CameraManager", Hs(offCameraManager));
    W(L"offsets", L"PlayerController.Pawn", Hs(offLocalPawn));
    W(L"offsets", L"CameraManager.CameraCache", Hs(offCameraCache));
    W(L"offsets", L"Level.Actors", Hs(offLevelActors));
    W(L"offsets", L"Actor.RootComponent", Hs(offActorRootComponent));
    W(L"offsets", L"Scene.ComponentToWorld", Hs(offComponentToWorld));
    W(L"offsets", L"Transform.Translation", Hs(offTransformTranslation));
    W(L"offsets", L"Mesh.Component", Hs(offMeshComponent));
    W(L"offsets", L"Mesh.ComponentToWorld", Hs(offMeshCtw));
    W(L"offsets", L"Mesh.BoxExtent", Hs(offMeshBounds));
    W(L"offsets", L"Mesh.Bones", Hs(offMeshBones));
    W(L"offsets", L"Mesh.Asset", Hs(offMeshAsset));
    W(L"offsets", L"Skeleton.BoneInfo", Hs(offBoneInfo));
    W(L"offsets", L"Skeleton.RefPose", Hs(offRefPose));
    W(L"offsets", L"Skeleton.PoseStride", Hs(refPoseStride));
    W(L"offsets", L"Skeleton.AnimBones", Hs(offAnimBones));
    W(L"offsets", L"Skeleton.AnimStride", Hs(animStride));
    W(L"offsets", L"Camera.FOV", Hs(offFov));

    W(L"filter", L"DinoClassHint", dinoClassHint);
    W(L"filter", L"MaxActors", Is(maxActors));
    W(L"filter", L"MaxDistanceM", Ds(maxDistanceM));
    W(L"runtime", L"ReaderHz", Is(readerHz));
    W(L"runtime", L"ActorRefreshMs", Is(actorRefreshMs));
    return true;
}

inline std::wstring Config::FirstMissing() const {
    if (!gworldSlot && gworldPattern.empty()) return L"roots.GWorldSlot / roots.GWorldPattern";
    if (!gnamesSlot && gnamesPattern.empty()) return L"roots.GNamesSlot / roots.GNamesPattern";
    if (!offActorRootComponent) return L"offsets.Actor.RootComponent";
    if (!offLevelActors) return L"offsets.Level.Actors";
    if (!offPersistentLevel) return L"offsets.World.PersistentLevel";
    return L"";
}

} // namespace claw











