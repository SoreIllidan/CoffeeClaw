// Coffeeclaw - UE5 object graph walker (read only)
#pragma once

#include "common.h"
#include "proc.h"
#include "config.h"
#include "pattern.h"
#include <algorithm>
#include <cwctype>
#include <utility>

namespace claw {

class UeReader {
public:
    void Bind(const Process* p, const Config* c) { proc_ = p; cfg_ = c; }
    void Reset() { gworld_ = 0; gnames_ = 0; classCache_.clear(); }

    bool ResolveRoots(std::wstring& log) {
        if (!proc_ || !cfg_) return false;
        gworld_ = 0;
        gnames_ = 0;
        classCache_.clear();

        uintptr_t slot = ResolveSlot(cfg_->gworldSlot, cfg_->gworldPattern,
                                     cfg_->gworldDispOffset, cfg_->gworldMatchLen, log, L"GWorld");
        if (!slot) return false;
        // Los valores del .ini pueden ser RVA (relativos al modulo) para que
        // sobrevivan a ASLR y reinicios. Una VA absoluta es mucho mayor.
        if (slot < proc_->base()) slot += proc_->base();
        {
            uintptr_t raw = 0;
            const bool got = proc_->ReadPtr(slot, raw);
            wchar_t tb[160];
            std::swprintf(tb, 160, L"GWorld slot=0x%llX leido=%d valor=0x%llX\n",
                          (unsigned long long)slot, got ? 1 : 0, (unsigned long long)raw);
            log += tb;
            if (got) gworld_ = raw;
        }
        if (!gworld_ || !PlausibleObject(gworld_)) {
            log += L"GWorld: el slot existe pero no contiene un puntero valido.\n";
            gworld_ = 0;
            return false;
        }

        slot = ResolveSlot(cfg_->gnamesSlot, cfg_->gnamesPattern,
                           cfg_->gnamesDispOffset, cfg_->gnamesMatchLen, log, L"GNames");
        if (!slot) return false;
        if (slot < proc_->base()) slot += proc_->base();
        if (cfg_->gnamesIndirection) {
            if (!proc_->ReadPtr(slot, gnames_)) { log += L"GNames: indireccion fallida.\n"; return false; }
        } else {
            gnames_ = slot;
        }
        if (!PlausibleObject(gnames_)) { log += L"GNames: pool no legible.\n"; gnames_ = 0; return false; }
        return true;
    }

    uintptr_t gworld() const { return gworld_; }
    uintptr_t gnames() const { return gnames_; }

    bool GetCamera(Vec3& loc, Rotator& rot, double& fov) const {
        if (!proc_ || !cfg_ || !gworld_) return false;
        uintptr_t gi = 0, players = 0, lp = 0, pc = 0, cam = 0;
        if (!proc_->ReadPtr(gworld_ + cfg_->offOwningGameInstance, gi)) return false;
        if (!ReadTArray(gi + cfg_->offLocalPlayers, players, nullptr)) return false;
        if (!proc_->ReadPtr(players, lp)) return false;
        if (!proc_->ReadPtr(lp + cfg_->offLocalPlayerCtrl, pc)) return false;
        if (!proc_->ReadPtr(pc + cfg_->offCameraManager, cam)) return false;

        struct Pov { double loc[3]; double rot[3]; float fov; float pad; };
        Pov pov{};
        if (!proc_->Read(cam + cfg_->offCameraCache, &pov, sizeof(pov))) return false;
        if (!std::isfinite(pov.loc[0]) || !std::isfinite(pov.rot[1])) return false;
        loc = Vec3(pov.loc[0], pov.loc[1], pov.loc[2]);
        rot = Rotator{ pov.rot[0], pov.rot[1], pov.rot[2] };
        fov = (pov.fov > 1.0f && pov.fov < 179.0f) ? (double)pov.fov : 90.0;
        return true;
    }

    // Direccion del POV de la camara (CameraCache), para el freecam.
    uintptr_t CameraPovAddr() const {
        if (!proc_ || !cfg_ || !gworld_) return 0;
        uintptr_t gi = 0, players = 0, lp = 0, pc = 0, cam = 0;
        if (!proc_->ReadPtr(gworld_ + cfg_->offOwningGameInstance, gi)) return 0;
        if (!ReadTArray(gi + cfg_->offLocalPlayers, players, nullptr)) return 0;
        if (!proc_->ReadPtr(players, lp)) return 0;
        if (!proc_->ReadPtr(lp + cfg_->offLocalPlayerCtrl, pc)) return 0;
        if (!proc_->ReadPtr(pc + cfg_->offCameraManager, cam)) return 0;
        if (!cam) return 0;
        return cam + cfg_->offCameraCache;
    }

    // Pawn del jugador local. Se prueban los offsets habituales por si el
    // build cambia; el primero que devuelve un actor con posicion valida gana.
    bool LocalPawn(uintptr_t& out) const {
        out = 0;
        if (!proc_ || !cfg_ || !gworld_) return false;
        uintptr_t gi = 0, players = 0, lp = 0, pc = 0;
        if (!proc_->ReadPtr(gworld_ + cfg_->offOwningGameInstance, gi)) return false;
        if (!ReadTArray(gi + cfg_->offLocalPlayers, players, nullptr)) return false;
        if (!proc_->ReadPtr(players, lp)) return false;
        if (!proc_->ReadPtr(lp + cfg_->offLocalPlayerCtrl, pc)) return false;
        if (!pc) return false;
        const uintptr_t cand[3] = { cfg_->offLocalPawn, 0x4E0, 0x4D0 };
        for (uintptr_t off : cand) {
            if (!off) continue;
            uintptr_t p = 0;
            if (!proc_->ReadPtr(pc + off, p) || !p || !PlausibleObject(p)) continue;
            Vec3 loc{};
            if (!ActorLocation(p, loc)) continue;
            if (std::fabs(loc.x) < 1.0 && std::fabs(loc.y) < 1.0) continue;
            out = p;
            return true;
        }
        return false;
    }

    // Escribe la posicion del actor: root component y, ademas, la posicion
    // interna que el CharacterMovement guarda y vuelca encima en cada tick.
    bool SetActorLocation(uintptr_t actor, const Vec3& loc) const {
        const double d[3] = { loc.x, loc.y, loc.z };
        bool ok = false;

        uintptr_t root = 0;
        if (proc_->ReadPtr(actor + cfg_->offActorRootComponent, root) && root) {
            ok |= proc_->Write(root + cfg_->offComponentToWorld + cfg_->offTransformTranslation, d, sizeof(d));
            // La relativa es la que manda: el componente recalcula el mundo desde ella.
            if (cfg_->offRelativeLocation) proc_->Write(root + cfg_->offRelativeLocation, d, sizeof(d));
        }

        uintptr_t mv = 0;
        if (cfg_->offMovementComp &&
            proc_->ReadPtr(actor + cfg_->offMovementComp, mv) && mv) {
            const uintptr_t offs[3] = { cfg_->offMovePos1, cfg_->offMovePos2, cfg_->offMovePos3 };
            for (uintptr_t off : offs) {
                if (!off) continue;
                ok |= proc_->Write(mv + off, d, sizeof(d));
            }
        }
        return ok;
    }

    bool EnumerateActors(std::vector<uintptr_t>& out) const {
        out.clear();
        if (!proc_ || !cfg_ || !gworld_) return false;
        uintptr_t level = 0;
        if (!proc_->ReadPtr(gworld_ + cfg_->offPersistentLevel, level)) return false;
        uintptr_t data = 0;
        int32_t count = 0;
        if (!ReadTArray(level + cfg_->offLevelActors, data, &count)) return false;
        if (count <= 0 || count > cfg_->maxActors) return false;
        std::vector<uint64_t> raw((size_t)count, 0);
        if (!proc_->Read(data, raw.data(), raw.size() * sizeof(uint64_t))) return false;
        out.reserve(raw.size());
        for (uint64_t v : raw) if (v) out.push_back((uintptr_t)v);
        return true;
    }

    // Caja real desde el mesh: centro y medias extensiones (FBoxSphereBounds).
    bool ActorBox(uintptr_t actor, Vec3& center, Vec3& extent) const {
        if (!proc_ || !cfg_ || !cfg_->offMeshComponent || !cfg_->offMeshBounds) return false;
        uintptr_t mesh = 0;
        if (!proc_->ReadPtr(actor + cfg_->offMeshComponent, mesh) || !mesh) return false;
        double c[3]{}, e[3]{};
        if (!proc_->Read(mesh + cfg_->offMeshCtw, c, sizeof(c))) return false;
        if (!proc_->Read(mesh + cfg_->offMeshBounds, e, sizeof(e))) return false;
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(c[i]) || !std::isfinite(e[i])) return false;
            if (std::fabs(e[i]) < 5 || std::fabs(e[i]) > 5000) return false;
        }
        center = Vec3(c[0], c[1], c[2]);
        extent = Vec3(e[0], e[1], e[2]);
        return true;
    }

    // Posiciones de mundo de cada hueso. Compone el cuaternion del mesh con la
    // traslacion local de cada FTransform del array.
    bool ReadBones(uintptr_t actor, std::vector<Vec3>& out) const {
        out.clear();
        if (!proc_ || !cfg_ || !cfg_->offMeshComponent || !cfg_->offMeshBones) return false;
        uintptr_t mesh = 0;
        if (!proc_->ReadPtr(actor + cfg_->offMeshComponent, mesh) || !mesh) return false;

        double q[4]{}, t[3]{};
        if (!proc_->Read(mesh + cfg_->offMeshCtw - 0x20, q, sizeof(q))) return false;
        if (!proc_->Read(mesh + cfg_->offMeshCtw, t, sizeof(t))) return false;
        double norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (!(norm > 0.5 && norm < 1.5)) return false;

        struct Raw { uint64_t data; int32_t num; int32_t max; };
        Raw arr{};
        if (!proc_->Read(mesh + cfg_->offMeshBones, &arr, sizeof(arr))) return false;
        if (arr.num <= 0 || arr.num > 1200 || !arr.data) return false;

        const size_t stride = 80;   // FTransform con quat double
        std::vector<uint8_t> buf((size_t)arr.num * stride);
        if (!proc_->Read((uintptr_t)arr.data, buf.data(), buf.size())) return false;

        auto rotate = [&](double vx, double vy, double vz, double& ox, double& oy, double& oz) {
            const double x = q[0], y = q[1], z = q[2], w = q[3];
            const double tx = 2.0 * (y * vz - z * vy);
            const double ty = 2.0 * (z * vx - x * vz);
            const double tz = 2.0 * (x * vy - y * vx);
            ox = vx + w * tx + (y * tz - z * ty);
            oy = vy + w * ty + (z * tx - x * tz);
            oz = vz + w * tz + (x * ty - y * tx);
        };

        out.reserve((size_t)arr.num);
        for (int i = 0; i < arr.num; ++i) {
            const uint8_t* p = buf.data() + (size_t)i * stride;
            double bq[4], bt[3];
            std::memcpy(bq, p, 32);
            std::memcpy(bt, p + 32, 24);
            if (!std::isfinite(bt[0]) || !std::isfinite(bt[1]) || !std::isfinite(bt[2])) continue;
            double rx = 0, ry = 0, rz = 0;
            rotate(bt[0], bt[1], bt[2], rx, ry, rz);
            out.push_back(Vec3(t[0] + rx, t[1] + ry, t[2] + rz));
        }
        return !out.empty();
    }

    // -------- esqueleto: pose de referencia + arbol de padres ----------------
    struct BoneLocal { int32_t parent = -1; float q[4]{}; float t[3]{}; };

    bool LoadSkeleton(uintptr_t asset, std::vector<BoneLocal>& out) const {
        out.clear();
        struct Raw { uint64_t data; int32_t num; int32_t max; };
        Raw info{}, pose{};
        if (!proc_->Read(asset + cfg_->offBoneInfo, &info, sizeof(info))) return false;
        if (!proc_->Read(asset + cfg_->offRefPose, &pose, sizeof(pose))) return false;
        if (info.num < 10 || info.num > 1200 || !info.data) return false;
        if (pose.num != info.num || !pose.data) return false;

        std::vector<uint8_t> ib((size_t)info.num * 16);
        if (!proc_->Read((uintptr_t)info.data, ib.data(), ib.size())) return false;
        const size_t stride = cfg_->refPoseStride;
        std::vector<uint8_t> pb((size_t)pose.num * stride);
        if (!proc_->Read((uintptr_t)pose.data, pb.data(), pb.size())) return false;

        out.resize((size_t)info.num);
        for (int i = 0; i < info.num; ++i) {
            int32_t parent = -1;
            std::memcpy(&parent, ib.data() + (size_t)i * 16 + 8, 4);
            BoneLocal& b = out[(size_t)i];
            b.parent = (parent >= 0 && parent < info.num && parent < i) ? parent : -1;
            const uint8_t* p = pb.data() + (size_t)i * stride;
            if (stride >= 80) {
                // FTransform: quaternion double (32) + traslacion double (24)
                double qd[4], td[3];
                std::memcpy(qd, p, 32);
                std::memcpy(td, p + 32, 24);
                for (int k = 0; k < 4; ++k) b.q[k] = (float)qd[k];
                for (int k = 0; k < 3; ++k) b.t[k] = (float)td[k];
            } else if (stride >= 64) {
                // quaternion float (16) + traslacion double (24)
                float qf[4];
                double td[3];
                std::memcpy(qf, p, 16);
                std::memcpy(td, p + 16, 24);
                for (int k = 0; k < 4; ++k) b.q[k] = qf[k];
                for (int k = 0; k < 3; ++k) b.t[k] = (float)td[k];
            } else {
                // todo float
                float qf[4], tf[3];
                std::memcpy(qf, p, 16);
                std::memcpy(tf, p + 16, 12);
                for (int k = 0; k < 4; ++k) b.q[k] = qf[k];
                for (int k = 0; k < 3; ++k) b.t[k] = tf[k];
            }
        }
        return true;
    }

    // Compone la pose de referencia con la transformada del mesh y devuelve
    // posiciones de mundo de cada hueso (mas el indice de su padre).
    bool ReadBonesWorld(uintptr_t actor, std::vector<Vec3>& points, std::vector<int32_t>& parents) {
        points.clear();
        parents.clear();
        uintptr_t mesh = 0;
        if (!proc_->ReadPtr(actor + cfg_->offMeshComponent, mesh) || !mesh) return false;
        uintptr_t asset = 0;
        if (!proc_->ReadPtr(mesh + cfg_->offMeshAsset, asset) || !asset) return false;

        const auto it = skelCache_.find(asset);
        if (it == skelCache_.end()) {
            std::vector<BoneLocal> sk;
            if (!LoadSkeleton(asset, sk)) return false;
            skelCache_[asset] = sk;
        }
        const std::vector<BoneLocal>& sk = skelCache_[asset];
        if (sk.empty()) return false;

        double cq[4]{}, ct[3]{};
        if (!proc_->Read(mesh + cfg_->offMeshCtw, ct, 24)) return false;
        bool okq = false;
        float cqf[4]{};
        if (proc_->Read(mesh + cfg_->offMeshCtw - 16, cqf, 16)) {
            const double nf = std::sqrt((double)cqf[0] * cqf[0] + (double)cqf[1] * cqf[1] +
                                        (double)cqf[2] * cqf[2] + (double)cqf[3] * cqf[3]);
            if (nf > 0.9 && nf < 1.1) {
                for (int k = 0; k < 4; ++k) cq[k] = cqf[k];
                okq = true;
            }
        }
        if (!okq) {
            if (!proc_->Read(mesh + cfg_->offMeshCtw - 32, cq, 32)) return false;
            const double nd = std::sqrt(cq[0] * cq[0] + cq[1] * cq[1] + cq[2] * cq[2] + cq[3] * cq[3]);
            if (!(nd > 0.9 && nd < 1.1)) return false;
        }

        // Array animado del componente: transformadas por hueso con paso 0x60
        // (cuaternion double en +0, traslacion double en +32, escala en +64).
        std::vector<BoneLocal> anim;
        bool useAnim = false;
        if (cfg_->offAnimBones && cfg_->animStride >= 0x50) {
            uintptr_t abData = 0;
            int32_t abNum = 0;
            if (ReadTArray(mesh + cfg_->offAnimBones, abData, &abNum) &&
                abNum >= 20 && abNum <= 1200 && abData) {
                std::vector<uint8_t> body((size_t)abNum * cfg_->animStride);
                if (proc_->Read(abData, body.data(), body.size())) {
                    anim.resize((size_t)abNum);
                    for (int i = 0; i < abNum; ++i) {
                        const uint8_t* p = body.data() + (size_t)i * cfg_->animStride;
                        double q[4], t3[3];
                        std::memcpy(q, p, 32);
                        std::memcpy(t3, p + 32, 24);
                        for (int k = 0; k < 4; ++k) anim[(size_t)i].q[k] = (float)q[k];
                        for (int k = 0; k < 3; ++k) anim[(size_t)i].t[k] = (float)t3[k];
                        anim[(size_t)i].parent = (i < (int)sk.size()) ? sk[(size_t)i].parent : -1;
                    }
                    useAnim = true;
                }
            }
        }
        const std::vector<BoneLocal>& src = useAnim ? anim : sk;
        boneInfo_ = L"";
        { wchar_t b[200]; std::swprintf(b, 200, L"huesos: animNum=%d src=%ls\\n", (int)anim.size(), useAnim ? L"anim" : L"refpose"); boneInfo_ += b; }

        // La pose puede venir ya en espacio de componente o relativa al padre.
        // Se calculan ambas y se elige la que da un tamano parecido al bounds.
        auto buildPositions = [&](bool accumulate, std::vector<Vec3>& pts) {
            pts.clear();
            std::vector<double> aq(src.size() * 4, 0.0), at(src.size() * 3, 0.0);
            for (size_t i = 0; i < src.size(); ++i) {
                const BoneLocal& b = src[i];
                double q[4] = { b.q[0], b.q[1], b.q[2], b.q[3] };
                double tt[3] = { b.t[0], b.t[1], b.t[2] };
                if (accumulate && b.parent >= 0) {
                    const double* pq = &aq[(size_t)b.parent * 4];
                    const double* pt = &at[(size_t)b.parent * 3];
                    double rx, ry, rz;
                    RotateBy(pq, tt[0], tt[1], tt[2], rx, ry, rz);
                    tt[0] = pt[0] + rx; tt[1] = pt[1] + ry; tt[2] = pt[2] + rz;
                    double wq[4];
                    MulQuat(pq, q, wq);
                    for (int k = 0; k < 4; ++k) q[k] = wq[k];
                }
                for (int k = 0; k < 4; ++k) aq[i * 4 + k] = q[k];
                for (int k = 0; k < 3; ++k) at[i * 3 + k] = tt[k];
                double rx, ry, rz;
                RotateBy(cq, tt[0], tt[1], tt[2], rx, ry, rz);
                pts.push_back(Vec3(ct[0] + rx, ct[1] + ry, ct[2] + rz));
            }
        };

        auto diagonal = [](const std::vector<Vec3>& pts) {
            if (pts.empty()) return 0.0;
            double mn[3] = { 1e18, 1e18, 1e18 }, mx[3] = { -1e18, -1e18, -1e18 };
            for (const Vec3& p : pts) {
                const double v[3] = { p.x, p.y, p.z };
                for (int k = 0; k < 3; ++k) {
                    mn[k] = std::min(mn[k], v[k]);
                    mx[k] = std::max(mx[k], v[k]);
                }
            }
            return std::sqrt((mx[0] - mn[0]) * (mx[0] - mn[0]) + (mx[1] - mn[1]) * (mx[1] - mn[1]) +
                             (mx[2] - mn[2]) * (mx[2] - mn[2]));
        };

        double ex[3]{};
        if (!proc_->Read(mesh + cfg_->offMeshBounds, ex, 24)) { ex[0] = ex[1] = ex[2] = 100.0; }
        double target = 2.0 * std::sqrt(ex[0] * ex[0] + ex[1] * ex[1] + ex[2] * ex[2]);
        if (!(target > 20.0 && target < 8000.0)) target = 400.0;

        std::vector<Vec3> direct, acc;
        buildPositions(false, direct);
        buildPositions(true, acc);
        const double dDirect = diagonal(direct);
        const double dAcc = diagonal(acc);
        // El array animado guarda transformadas en espacio de hueso (relativas\n        // al padre), asi que hay que acumular siempre. La pose de referencia\n        // del asset puede venir ya en espacio de componente.\n        points = useAnim ? acc\n                         : ((std::fabs(dAcc - target) < std::fabs(dDirect - target)) ? acc : direct);
        { wchar_t b[220]; std::swprintf(b, 220, L"diag: direct=%.1f accum=%.1f target=%.1f\\n", dDirect, dAcc, target); boneInfo_ += b; }
        parents.clear();
        for (const BoneLocal& b : src) parents.push_back(b.parent);
        return !points.empty();
    }

    static void RotateBy(const double* q, double vx, double vy, double vz,
                         double& ox, double& oy, double& oz) {
        const double x = q[0], y = q[1], z = q[2], w = q[3];
        const double tx = 2.0 * (y * vz - z * vy);
        const double ty = 2.0 * (z * vx - x * vz);
        const double tz = 2.0 * (x * vy - y * vx);
        ox = vx + w * tx + (y * tz - z * ty);
        oy = vy + w * ty + (z * tx - x * tz);
        oz = vz + w * tz + (x * ty - y * tx);
    }

    static void MulQuat(const double* a, const double* b, double* out) {
        out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
        out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
        out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
        out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    }

    bool ActorLocation(uintptr_t actor, Vec3& out) const {
        uintptr_t root = 0;
        if (!proc_->ReadPtr(actor + cfg_->offActorRootComponent, root) || !root) return false;
        double xyz[3]{};
        if (!proc_->Read(root + cfg_->offComponentToWorld + cfg_->offTransformTranslation, xyz, sizeof(xyz))) return false;
        if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2])) return false;
        out = Vec3(xyz[0], xyz[1], xyz[2]);
        return true;
    }

    // Resolves UObject::NamePrivate through the FName pool.
    bool ObjectName(uintptr_t obj, std::wstring& out) const {
        out.clear();
        if (!obj || !gnames_) return false;
        int32_t index = 0;
        if (!proc_->ReadT(obj + cfg_->offNamePrivate, index) || index <= 0) return false;
        return NameFromIndex(index, out);
    }

    bool NameFromIndex(int32_t index, std::wstring& out) const {
        out.clear();
        if (!gnames_ || index <= 0) return false;
        const uint32_t block = (uint32_t)index >> 16;
        const uint32_t offset = (uint32_t)index & 0xFFFF;
        if (block > 8191) return false;
        uintptr_t blockPtr = 0;
        if (!proc_->ReadPtr(gnames_ + (uintptr_t)block * 8, blockPtr) || !blockPtr) return false;
        // Entrada: uint16 header = bIsWide:1 | ProbeHash:5 | Len:10, texto SIN
        // terminador en +2. Formato verificado en el build 94.34.0 de ASA.
        const uintptr_t entry = blockPtr + (uintptr_t)offset * 2;
        uint16_t h = 0;
        if (!proc_->Read(entry, &h, sizeof(h))) return false;
        const bool wide = (h & 1) != 0;
        int len = (int)((h >> 6) & 0x3FF);
        if (len <= 0 || len > 512) len = 255;
        if (wide) {
            wchar_t buf[520]{};
            if (!proc_->Read(entry + 2, buf, (size_t)len * 2)) return false;
            for (int i = 0; i < len && buf[i]; ++i) out.push_back(buf[i]);
        } else {
            char buf[520]{};
            if (!proc_->Read(entry + 2, buf, (size_t)len)) return false;
            for (int i = 0; i < len && buf[i]; ++i) out.push_back((wchar_t)(unsigned char)buf[i]);
        }
        return !out.empty();
    }

    // Walks ClassPrivate -> SuperStruct chain and tests every name against the hint.
    bool IsDinoClass(uintptr_t actor, std::wstring& className) const {
        className.clear();
        uintptr_t cls = 0;
        if (!proc_->ReadPtr(actor + cfg_->offClassPrivate, cls) || !cls) return false;

        auto it = classCache_.find(cls);
        if (it != classCache_.end()) {
            className = it->second.name;
            return it->second.isDino;
        }

        bool isDino = false;
        std::wstring firstName;
        uintptr_t cur = cls;
        for (int depth = 0; depth < 40 && cur; ++depth) {
            std::wstring nm;
            if (!ObjectName(cur, nm)) break;
            if (firstName.empty()) firstName = nm;
            if (!cfg_->dinoClassHint.empty()) {
                std::wstring lower = Lower(nm), hint = Lower(cfg_->dinoClassHint);
                if (lower.find(hint) != std::wstring::npos) { isDino = true; break; }
            }
            uintptr_t super = 0;
            if (!proc_->ReadPtr(cur + cfg_->offSuperStruct, super)) break;
            cur = super;
        }
        className = firstName.empty() ? L"<sin nombre>" : firstName;
        // Plan B sin depender de SuperStruct: en ARK los dinosaurios son
        // Blueprints con nombre "..._Character_BP_C".
        if (!isDino && firstName.find(L"_Character_BP") != std::wstring::npos) isDino = true;
        classCache_[cls] = { className, isDino };
        return isDino;
    }

    // Sanity walk used by the diagnostic button. Returns a human readable report.
    std::wstring BoneInfo() const { return boneInfo_; }

    std::wstring Diagnose() const {
        std::wstring log;
        if (!proc_ || !cfg_) return L"Sin proceso.";
        log += L"proceso: " + cfg_->processName + L"\n";
        log += L"base: 0x" + Hex(proc_->base()) + L"  imagen: " + std::to_wstring(proc_->imageSize()) + L" bytes\n";

        if (!gworld_) { log += L"GWorld: no resuelto\n"; return log; }
        log += L"GWorld: 0x" + Hex(gworld_) + L"\n";
        if (gnames_) log += L"GNames: 0x" + Hex(gnames_) + L"\n";

        std::wstring worldName;
        if (ObjectName(gworld_, worldName)) log += L"  nombre: " + worldName + L"\n";

        uintptr_t level = 0;
        if (!proc_->ReadPtr(gworld_ + cfg_->offPersistentLevel, level) || !level) {
            log += L"  World.PersistentLevel: FALLA\n";
        } else {
            log += L"  PersistentLevel: 0x" + Hex(level) + L"\n";
            uintptr_t data = 0;
            int32_t count = 0;
            if (ReadTArray(level + cfg_->offLevelActors, data, &count)) {
                log += L"  Level.Actors: " + std::to_wstring(count) + L" entradas\n";
            } else {
                log += L"  Level.Actors: FALLA\n";
            }
        }

        Vec3 cam; Rotator rot; double fov = 0;
        if (GetCamera(cam, rot, fov)) {
            log += L"  camara: " + Fmt(cam.x) + L", " + Fmt(cam.y) + L", " + Fmt(cam.z) +
                   L"  yaw " + Fmt(rot.yaw) + L"  fov " + Fmt(fov) + L"\n";
        } else {
            log += L"  cadena de camara: FALLA (revisa LocalPlayers / PlayerController / CameraCache)\n";
        }

        std::vector<uintptr_t> actors;
        if (EnumerateActors(actors)) {
            std::unordered_map<std::wstring, int> names;
            int dinoHits = 0;
            int moved = 0;
            int checked = 0;
            // Primero la clase (barata), luego la posicion: asi no gastamos
            // lecturas en los miles de volumenes del nivel.
            for (size_t i = 0; i < actors.size() && checked < 800; ++i) {
                std::wstring cls;
                const bool isDino = IsDinoClass(actors[i], cls);
                if (!cls.empty()) names[cls] += 1;
                if (isDino) ++dinoHits;
                if (!isDino) continue;
                ++checked;
                Vec3 loc;
                if (ActorLocation(actors[i], loc)) ++moved;
            }
            // Traza sobre actores de clase dinosaurio.
            log += L"  traza de dinosaurios:\n";
            int traced = 0;
            for (size_t i = 0; i < actors.size() && traced < 3; ++i) {
                std::wstring clsName;
                if (!IsDinoClass(actors[i], clsName)) continue;
                uintptr_t cls = 0, root = 0;
                int32_t nameIdx = 0;
                proc_->ReadPtr(actors[i] + cfg_->offClassPrivate, cls);
                if (cls) proc_->ReadT(cls + cfg_->offNamePrivate, nameIdx);
                std::wstring nm;
                NameFromIndex(nameIdx, nm);
                const bool gotRoot = proc_->ReadPtr(actors[i] + cfg_->offActorRootComponent, root);
                uintptr_t comp = 0;
                if (gotRoot) proc_->ReadPtr(actors[i] + cfg_->offActorRootComponent, comp);
                Vec3 loc;
                const bool gotPos = ActorLocation(actors[i], loc);
                wchar_t buf[360];
                std::swprintf(buf, 360,
                              L"    actor=0x%llX idx=%d '%ls' root=%d/0x%llX pos=%d (%.1f %.1f %.1f)\n",
                              (unsigned long long)actors[i], nameIdx, nm.c_str(),
                              gotRoot ? 1 : 0, (unsigned long long)root, gotPos ? 1 : 0,
                              loc.x, loc.y, loc.z);
                log += buf;
                ++traced;
            }
            log += L"  actores con transform legible: " + std::to_wstring(moved) + L" de " +
                   std::to_wstring(checked) + L" dinos revisados\n";
            log += L"  coincidencias con '" + cfg_->dinoClassHint + L"': " + std::to_wstring(dinoHits) + L"\n";
            log += L"  clases vistas (top 12):\n";
            std::vector<std::pair<std::wstring, int>> sorted(names.begin(), names.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            for (size_t i = 0; i < sorted.size() && i < 12; ++i) {
                log += L"    " + sorted[i].first + L"  x" + std::to_wstring(sorted[i].second) + L"\n";
            }
        } else {
            log += L"  no pude recorrer actores.\n";
        }
        return log;
    }


    // Acceso publico al lector de TArray (lo usan las herramientas).
    bool ReadArray(uintptr_t addr, uintptr_t& data, int32_t* countOut) const {
        return ReadTArray(addr, data, countOut);
    }

private:
    struct Cached { std::wstring name; bool isDino; };

    static std::wstring Lower(std::wstring s) {
        for (auto& ch : s) ch = (wchar_t)::towlower(ch);
        return s;
    }
    static std::wstring Hex(uintptr_t v) {
        wchar_t buf[32];
        std::swprintf(buf, 32, L"%llX", (unsigned long long)v);
        return buf;
    }
    static std::wstring Fmt(double v) {
        wchar_t buf[32];
        std::swprintf(buf, 32, L"%.1f", v);
        return buf;
    }

    bool PlausibleObject(uintptr_t addr) const {
        if (addr < 0x10000 || (addr & 0x7)) return false;
        int64_t probe = 0;
        return proc_->Read(addr, &probe, sizeof(probe));
    }

    bool ReadTArray(uintptr_t addr, uintptr_t& data, int32_t* countOut) const {
        struct TArrayRaw { uint64_t data; int32_t num; int32_t max; };
        TArrayRaw arr{};
        if (!proc_->Read(addr, &arr, sizeof(arr))) return false;
        if (arr.num < 0 || arr.num > 2000000) return false;
        data = (uintptr_t)arr.data;
        if (countOut) *countOut = arr.num;
        return data != 0;
    }

    uintptr_t ResolveSlot(uintptr_t absolute, const std::string& patternText, int dispOffset,
                          int matchLen, std::wstring& log, const wchar_t* label) const {
        if (absolute) return absolute;
        if (patternText.empty()) {
            log += std::wstring(label) + L": sin slot ni pattern en offsets.ini\n";
            return 0;
        }
        std::vector<int> pat;
        if (!ParsePattern(patternText.c_str(), pat)) {
            log += std::wstring(label) + L": pattern invalido\n";
            return 0;
        }
        std::vector<uint8_t> image = proc_->ReadImage(0x20000000);
        if (image.empty()) {
            log += std::wstring(label) + L": no pude leer la imagen del modulo\n";
            return 0;
        }
        size_t off = ScanPattern(image.data(), image.size(), pat);
        if (!off) {
            log += std::wstring(label) + L": pattern sin coincidencias en el modulo\n";
            return 0;
        }
        uintptr_t match = proc_->base() + off;
        uintptr_t slot = ResolveRip(match, off, image.data(), image.size(), dispOffset, matchLen);
        if (!slot) {
            log += std::wstring(label) + L": RIP sin resolver\n";
            return 0;
        }
        return slot;
    }

    const Process* proc_ = nullptr;
    const Config* cfg_ = nullptr;
    uintptr_t gworld_ = 0;
    uintptr_t gnames_ = 0;
    mutable std::unordered_map<uintptr_t, Cached> classCache_;
    mutable std::unordered_map<uintptr_t, std::vector<BoneLocal>> skelCache_;
    mutable std::wstring boneInfo_;
};

} // namespace claw













