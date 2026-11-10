// NSMBW water surface: d3d::rtexQuadManyTwin_c::drawTexQuadManyTwin (0x8000D000) natively, so that
// with NSMBW_WATERHLE=1 the ordinary water surface (drawTexQuadManyTwinNormal, 0x8000B530) is drawn
// as one GXBegin instead of one per quad.
//
// drawTexQuadManyTwinNormal sets up its GX state (two textures, two TEV stages, fog off, the camera's
// view matrix, clipping off), then draws `count` quads, each its own GXBegin(GX_QUADS, GX_VTXFMT0, 4)
// with every vertex component written to the FIFO one by one: in 4-1 with two players that is about
// a fifth of the game thread, nearly all of it per-draw work in the HLE FIFO path. Nothing changes
// GX state between those GXBegins, so GX_QUADS with 4 * count vertices draws exactly the same
// triangles in the same order (Aurora triangulates every quad on its own and merges the separate
// draws back into one anyway).
//
// With the switch on, drawTexQuadManyTwinNormal below repeats the guest function call for call - the
// same GX HLE entry points and translated callees with the same arguments, the same stack frame and
// stack contents - and only replaces the quad loop: it builds the very bytes the loop would write
// (positions bit for bit from the guest's arrays, the two colours and texture coordinates as the
// loop computes them) and hands them to one GXBegin through GX_HLE_FIFO_WriteBurst, which feeds the
// raw-begin path (HleTryBeginRaw / RawBeginAppend / TrySubmitRawDirectFifoDraw) or a display list
// exactly as the per-component writes do. Switch off (the default), drawTexQuadManyTwin dispatches
// to the translated 0x8000B530 as the translated dispatcher did.
//
// The override is on the dispatcher rather than on 0x8000B530 itself so that the translated
// drawTexQuadManyTwinNormal stays in the build for the default path. This file builds with the
// translator's strict float options (CMakeLists.txt), as the vertex positions pass through the same
// float -> double -> float conversions as in the translated loop.
//
// NSMBW_WATERHLE_SHOTS=<frame>[,<frame>...] (Linux validation aid): writes Aurora frame captures
// waterhle_<frame>.bmp into NSMBW_WATERHLE_SHOT_DIR (default: the working directory) while a water
// surface is being drawn, for comparing runs with and without the switch.
#include "abi_bridge.h"
#include "hle_stubs.h"
#include "memory.h"
#include "runtime_log.h"

#include <aurora/aurora.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

extern "C" void GX_HLE_FIFO_WriteBurst(const uint8_t* data, uint32_t sizeBytes);

namespace aurora::gfx {
uint32_t current_frame() noexcept;
}

namespace {

// The guest functions this one dispatches to and calls.
constexpr uint32_t kDrawNormal = 0x8000B530u;   // drawTexQuadManyTwinNormal
constexpr uint32_t kDrawMagma = 0x8000BA70u;    // drawTexQuadManyTwinMagma
constexpr uint32_t kDrawSand = 0x8000C020u;     // drawTexQuadManyTwinSand
constexpr uint32_t kDrawMagmaHi = 0x8000C5C0u;  // drawTexQuadManyTwinMagmaHi
constexpr uint32_t kDrawPoison = 0x8000CA50u;   // drawTexQuadManyTwinPoison
constexpr uint32_t kResetMaterial = 0x80164F50u;     // m3d::resetMaterial
constexpr uint32_t kGetCurrentCamera = 0x80164B30u;  // m3d::getCurrentCamera
constexpr uint32_t kGetCameraMtx = 0x802540B0u;      // nw4r::g3d::Camera::GetCameraMtx
constexpr uint32_t kGetViewMtx = 0x8016A190u;        // m3d::scnLeaf_c::getViewMtx
constexpr uint32_t kPsMtxConcat = 0x801C0540u;       // PSMTXConcat
constexpr uint32_t kGxSetFogRangeAdj = 0x801C8CB0u;  // translated, not HLE

// rtexQuadManyTwin_c: two GXTexObjs, then the four corner arrays (1100 x Vec each), then the
// colours, the quad count, the kind and the two texture-coordinate offsets.
constexpr uint32_t kTexObj0 = 8;
constexpr uint32_t kTexObj1 = 40;
constexpr uint32_t kCornerA = 72;
constexpr uint32_t kCornerB = 13272;
constexpr uint32_t kCornerC = 26472;
constexpr uint32_t kCornerD = 39672;
constexpr uint32_t kColorTop = 52872;     // GXColor of corners A and B
constexpr uint32_t kColorBottom = 52876;  // GXColor of corners C and D
constexpr uint32_t kQuadCount = 52880;
constexpr uint32_t kKind = 52884;
constexpr uint32_t kTexOfs0 = 52888;  // s16 added to the S coordinate of TEX0
constexpr uint32_t kTexOfs1 = 52890;  // and of TEX1

// One vertex of GX_VTXFMT0 as the function sets it up: POS xyz f32, CLR0 RGBA8, TEX0 and TEX1 st s16.
constexpr uint32_t kVertexBytes = 12u + 4u + 4u + 4u;
// A GXBegin counts vertices in 16 bits.
constexpr int32_t kMaxQuadsPerBegin = 0xFFFF / 4;

bool WaterHleEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("NSMBW_WATERHLE");
        const bool on = value != nullptr && value[0] == '1';
        if (on) {
            RT_LOGF(RT_TAG_GX, "NSMBW_WATERHLE=1: water surface quads drawn as one GXBegin\n");
        }
        return on;
    }();
    return enabled;
}

void SetArgs(CpuContext* ctx, std::initializer_list<uint32_t> args) {
    uint32_t reg = 3;
    for (const uint32_t arg : args) {
        ctx->gpr[reg++] = arg;
    }
}

void PutBe32(uint8_t*& out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
    out += 4;
}

void PutBe16(uint8_t*& out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value >> 8);
    out[1] = static_cast<uint8_t>(value);
    out += 2;
}

// The loop's lfs + GX_HLE_FIFO_WriteFloat(static_cast<float>(f.d)): the same conversions, built with
// the same float options as the translated code.
void PutGuestFloat(uint8_t*& out, uint32_t address) {
    const double loaded = MemoryInline::FlatReadFloat32(address);
    const float written = static_cast<float>(loaded);
    uint32_t bits = 0;
    std::memcpy(&bits, &written, sizeof(bits));
    PutBe32(out, bits);
}

// One vertex as the loop writes it: position, colour (its four bytes in memory order), then TEX0 and
// TEX1 as (s, t) with s = base + the object's offset, truncated to 16 bits as sth does.
void PutVertex(uint8_t*& out, uint32_t position, uint32_t color, int32_t s0, int32_t s1, uint32_t t) {
    PutGuestFloat(out, position);
    PutGuestFloat(out, position + 4);
    PutGuestFloat(out, position + 8);
    PutBe32(out, color);
    PutBe16(out, static_cast<uint32_t>(s0));
    PutBe16(out, t);
    PutBe16(out, static_cast<uint32_t>(s1));
    PutBe16(out, t);
}

// The quad loop of 0x8000B530 (0x8000B874-0x8000BA48) as GX_QUADS with 4 * count vertices.
void DrawQuadsMerged(CpuContext* ctx, uint32_t self) {
    // cmpwi count, 0 / beq skips the loop; the loop runs while i < count (signed).
    const int32_t count = static_cast<int32_t>(MemoryInline::FlatRead32(self + kQuadCount));
    if (count <= 0) {
        return;
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        RT_LOGF(RT_TAG_GX, "NSMBW_WATERHLE: first water surface, %d quads in one GXBegin\n", count);
    }
    const uint32_t colorTop = MemoryInline::FlatRead32(self + kColorTop);
    const uint32_t colorBottom = MemoryInline::FlatRead32(self + kColorBottom);
    const int32_t ofs0 = static_cast<int16_t>(MemoryInline::FlatRead16(self + kTexOfs0));
    const int32_t ofs1 = static_cast<int16_t>(MemoryInline::FlatRead16(self + kTexOfs1));
    constexpr uint32_t kTop = 0;
    constexpr uint32_t kBottom = 1023;

    static std::vector<uint8_t> bytes;  // game thread only
    for (int32_t first = 0; first < count; first += kMaxQuadsPerBegin) {
        const int32_t quads = std::min(count - first, kMaxQuadsPerBegin);
        bytes.resize(static_cast<size_t>(quads) * 4u * kVertexBytes);
        uint8_t* out = bytes.data();
        for (int32_t i = first; i < first + quads; ++i) {
            // r21 = (i % 11) * 93 (mulhw by 0x2E8BA2E9 / srawi 1: i / 11), its right edge r21 + 92.
            const int32_t left = (i % 11) * 93;
            const int32_t right = left + 92;
            const uint32_t step = static_cast<uint32_t>(i) * 12u;
            PutVertex(out, self + kCornerA + step, colorTop, left + ofs0, left + ofs1, kTop);
            PutVertex(out, self + kCornerB + step, colorTop, right + ofs0, right + ofs1, kTop);
            PutVertex(out, self + kCornerC + step, colorBottom, right + ofs0, right + ofs1, kBottom);
            PutVertex(out, self + kCornerD + step, colorBottom, left + ofs0, left + ofs1, kBottom);
        }
        SetArgs(ctx, {0x80u /* GX_QUADS */, 0u /* GX_VTXFMT0 */, static_cast<uint32_t>(quads) * 4u});
        InvokeDirectCpu<0x801C5570u>(ctx);  // GXBegin
        GX_HLE_FIFO_WriteBurst(bytes.data(), static_cast<uint32_t>(bytes.size()));
    }
}

// NSMBW_WATERHLE_SHOTS: requests the next listed Aurora frame capture once the previous one is past.
void ServiceShots() {
    static const std::vector<uint32_t> frames = [] {
        std::vector<uint32_t> parsed;
        if (const char* list = std::getenv("NSMBW_WATERHLE_SHOTS")) {
            for (const char* p = list; *p != '\0';) {
                char* end = nullptr;
                const unsigned long frame = std::strtoul(p, &end, 10);
                if (end == p) break;
                parsed.push_back(static_cast<uint32_t>(frame));
                p = *end == ',' ? end + 1 : end;
            }
            std::sort(parsed.begin(), parsed.end());
        }
        return parsed;
    }();
    if (frames.empty()) {
        return;
    }
    static size_t next = 0;
    static bool requested = false;
    static std::string path;
    const uint32_t now = aurora::gfx::current_frame();
    while (next < frames.size()) {
        if (requested && now > frames[next] + 2u) {
            ++next;
            requested = false;
            continue;
        }
        if (!requested) {
            if (now >= frames[next]) {  // too late for this one
                ++next;
                continue;
            }
            const char* dir = std::getenv("NSMBW_WATERHLE_SHOT_DIR");
            path = std::string(dir != nullptr ? dir : ".") + "/waterhle_" + std::to_string(frames[next]) + ".bmp";
            aurora_request_frame_capture(frames[next], path.c_str());
            requested = true;
        }
        break;
    }
}

// d3d::rtexQuadManyTwin_c::drawTexQuadManyTwinNormal (0x8000B530), call for call; see the top.
void DrawNormalHle(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    const uint32_t callerSp = ctx->gpr[1];
    const uint32_t r2 = ctx->gpr[2];

    // stwu r1, -160(r1); mflr r0; stw r0, 164(r1); _savegpr_21
    const uint32_t sp = callerSp - 160u;
    MemoryInline::FlatWriteRam32(sp, callerSp);
    MemoryInline::FlatWriteRam32(sp + 164u, ctx->lr);
    for (uint32_t reg = 21; reg <= 31; ++reg) {
        MemoryInline::FlatWriteRam32(sp + 116u + (reg - 21u) * 4u, ctx->gpr[reg]);
    }
    ctx->gpr[1] = sp;
    ctx->gpr[21] = self;  // the guest keeps `this` in r21 for the whole function

    SetArgs(ctx, {self + kTexObj0, 0});
    ctx->lr = 0x8000B554u;
    InvokeDirectCpu<0x801C74C0u>(ctx);  // GXLoadTexObj(&mTexObj0, GX_TEXMAP0)
    SetArgs(ctx, {self + kTexObj1, 1});
    ctx->lr = 0x8000B560u;
    InvokeDirectCpu<0x801C74C0u>(ctx);  // GXLoadTexObj(&mTexObj1, GX_TEXMAP1)
    InvokeDirectCpu<0x801C4070u>(ctx);  // GXClearVtxDesc
    SetArgs(ctx, {9, 1});
    InvokeDirectCpu<0x801C37C0u>(ctx);  // GXSetVtxDesc(GX_VA_POS, GX_DIRECT)
    SetArgs(ctx, {11, 1});
    InvokeDirectCpu<0x801C37C0u>(ctx);  // GX_VA_CLR0
    SetArgs(ctx, {13, 1});
    InvokeDirectCpu<0x801C37C0u>(ctx);  // GX_VA_TEX0
    SetArgs(ctx, {14, 1});
    InvokeDirectCpu<0x801C37C0u>(ctx);  // GX_VA_TEX1
    SetArgs(ctx, {0, 9, 1, 4, 0});
    InvokeDirectCpu<0x801C40B0u>(ctx);  // GXSetVtxAttrFmt(0, POS, XYZ, F32, 0)
    SetArgs(ctx, {0, 11, 1, 5, 0});
    InvokeDirectCpu<0x801C40B0u>(ctx);  // CLR0, RGBA, RGBA8
    SetArgs(ctx, {0, 13, 1, 3, 10});
    InvokeDirectCpu<0x801C40B0u>(ctx);  // TEX0, ST, S16, 10
    SetArgs(ctx, {0, 14, 1, 3, 10});
    InvokeDirectCpu<0x801C40B0u>(ctx);  // TEX1, ST, S16, 10
    ctx->lr = 0x8000B5F8u;
    InvokeIndirectCpu(kResetMaterial, ctx);
    SetArgs(ctx, {1});
    InvokeDirectCpu<0x801C6AC0u>(ctx);  // GXSetNumChans(1)
    SetArgs(ctx, {4, 0, 1, 1, 0, 0, 2});
    InvokeDirectCpu<0x801C6AF0u>(ctx);  // GXSetChanCtrl(GX_COLOR0A0, off, VTX, VTX, 0, NONE, NONE)
    SetArgs(ctx, {2});
    InvokeDirectCpu<0x801C4A20u>(ctx);  // GXSetNumTexGens(2)
    SetArgs(ctx, {0, 1, 4, 60, 0, 125});
    ctx->lr = 0x8000B644u;
    InvokeDirectCpu<0x801C47D0u>(ctx);  // GXSetTexCoordGen2(TEXCOORD0, MTX2x4, TEX0, IDENTITY, ...)
    SetArgs(ctx, {1, 1, 5, 60, 0, 125});
    ctx->lr = 0x8000B660u;
    InvokeDirectCpu<0x801C47D0u>(ctx);  // TEXCOORD1 from TEX1
    SetArgs(ctx, {2});
    InvokeDirectCpu<0x801C88F0u>(ctx);  // GXSetNumTevStages(2)
    SetArgs(ctx, {0});
    InvokeDirectCpu<0x801C8110u>(ctx);  // GXSetNumIndStages(0)
    SetArgs(ctx, {0, 0, 1, 2, 3});
    InvokeDirectCpu<0x801C8640u>(ctx);  // GXSetTevSwapModeTable
    SetArgs(ctx, {0, 0, 0, 4});
    InvokeDirectCpu<0x801C8790u>(ctx);  // GXSetTevOrder(0, ...)
    SetArgs(ctx, {0, 15, 15, 15, 8});
    InvokeDirectCpu<0x801C82F0u>(ctx);  // GXSetTevColorIn(0, ...)
    SetArgs(ctx, {0, 0, 0, 0, 1, 0});
    InvokeDirectCpu<0x801C8370u>(ctx);  // GXSetTevColorOp(0, ...)
    SetArgs(ctx, {0, 7, 5, 4, 7});
    InvokeDirectCpu<0x801C8330u>(ctx);  // GXSetTevAlphaIn(0, ...)
    SetArgs(ctx, {0, 0, 0, 0, 1, 0});
    InvokeDirectCpu<0x801C83D0u>(ctx);  // GXSetTevAlphaOp(0, ...)
    SetArgs(ctx, {1, 1, 1, 4});
    InvokeDirectCpu<0x801C8790u>(ctx);  // GXSetTevOrder(1, ...)
    SetArgs(ctx, {1, 15, 0, 8, 10});
    InvokeDirectCpu<0x801C82F0u>(ctx);  // GXSetTevColorIn(1, ...)
    SetArgs(ctx, {1, 0, 0, 0, 1, 0});
    InvokeDirectCpu<0x801C8370u>(ctx);  // GXSetTevColorOp(1, ...)
    SetArgs(ctx, {1, 7, 0, 4, 7});
    InvokeDirectCpu<0x801C8330u>(ctx);  // GXSetTevAlphaIn(1, ...)
    SetArgs(ctx, {1, 0, 0, 0, 1, 0});
    InvokeDirectCpu<0x801C83D0u>(ctx);  // GXSetTevAlphaOp(1, ...)
    SetArgs(ctx, {0});
    InvokeDirectCpu<0x801C8EB0u>(ctx);  // GXSetZCompLoc(GX_FALSE)
    SetArgs(ctx, {1, 4, 5, 15});
    InvokeDirectCpu<0x801C8DC0u>(ctx);  // GXSetBlendMode(BLEND, SRCALPHA, INVSRCALPHA, SET)
    SetArgs(ctx, {1, 3, 0});
    InvokeDirectCpu<0x801C8E70u>(ctx);  // GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE)
    SetArgs(ctx, {4, 0, 1, 4, 0});
    InvokeDirectCpu<0x801C86C0u>(ctx);  // GXSetAlphaCompare(GREATER, 0, OR, GREATER, 0)
    SetArgs(ctx, {1});
    InvokeDirectCpu<0x801C9D60u>(ctx);  // GXSetClipMode(GX_CLIP_DISABLE)

    // GXSetFog(GX_FOG_NONE, 0, 0, 0, 0, {0}): the colour is a zeroed word at 8(r1).
    const double zero = MemoryInline::FlatReadFloat32(r2 - 32736u);
    ctx->gpr[27] = 0;
    MemoryInline::FlatWriteRam32(sp + 8u, 0);
    ctx->fpr[1].d = zero;
    ctx->fpr[2].d = zero;
    ctx->fpr[3].d = zero;
    ctx->fpr[4].d = zero;
    SetArgs(ctx, {0, sp + 8u});
    InvokeDirectCpu<0x801C8920u>(ctx);  // GXSetFog
    SetArgs(ctx, {0, 0, 0});
    InvokeIndirectCpu(kGxSetFogRangeAdj, ctx);  // GXSetFogRangeAdj(GX_FALSE, 0, NULL)
    SetArgs(ctx, {0});
    InvokeDirectCpu<0x801C5860u>(ctx);  // GXSetCullMode(GX_CULL_NONE)
    SetArgs(ctx, {1});
    InvokeDirectCpu<0x801C8F90u>(ctx);  // GXSetDither(GX_TRUE)

    // The camera matrix with its translation zeroed, times the object's view matrix, as the
    // position matrix.
    ctx->lr = 0x8000B814u;
    InvokeIndirectCpu(kGetCurrentCamera, ctx);
    MemoryInline::FlatWriteRam32(sp + 12u, ctx->gpr[3]);
    SetArgs(ctx, {sp + 12u, sp + 64u});
    ctx->lr = 0x8000B824u;
    InvokeIndirectCpu(kGetCameraMtx, ctx);
    const double zeroAgain = MemoryInline::FlatReadFloat32(r2 - 32736u);
    ctx->fpr[0].d = zeroAgain;
    MemoryInline::FlatWriteRamFloat32(sp + 76u, zeroAgain);
    MemoryInline::FlatWriteRamFloat32(sp + 92u, zeroAgain);
    MemoryInline::FlatWriteRamFloat32(sp + 108u, zeroAgain);
    SetArgs(ctx, {self, sp + 16u});
    ctx->lr = 0x8000B840u;
    InvokeIndirectCpu(kGetViewMtx, ctx);
    SetArgs(ctx, {sp + 16u, sp + 64u, sp + 16u});
    InvokeIndirectCpu(kPsMtxConcat, ctx);
    SetArgs(ctx, {sp + 16u, 0});
    InvokeDirectCpu<0x801C9940u>(ctx);  // GXLoadPosMtxImm(mtx, GX_PNMTX0)
    SetArgs(ctx, {0});
    InvokeDirectCpu<0x801C9A60u>(ctx);  // GXSetCurrentMtx(GX_PNMTX0)

    ctx->gpr[22] = 0;
    ctx->gpr[28] = self + 0x10000u;
    DrawQuadsMerged(ctx, self);

    SetArgs(ctx, {0});
    InvokeDirectCpu<0x801C9D60u>(ctx);  // GXSetClipMode(GX_CLIP_ENABLE)

    // _restgpr_21; lwz r0, 164(r1); mtlr r0; addi r1, r1, 160
    for (uint32_t reg = 21; reg <= 31; ++reg) {
        ctx->gpr[reg] = MemoryInline::FlatRead32(sp + 116u + (reg - 21u) * 4u);
    }
    const uint32_t lr = MemoryInline::FlatRead32(sp + 164u);
    ctx->gpr[0] = lr;
    ctx->lr = lr;
    ctx->gpr[11] = callerSp;
    ctx->gpr[1] = callerSp;
}

void CallTranslated(CpuContext* ctx, uint32_t target, uint32_t r0, uint32_t r4, uint32_t r5, uint32_t cr) {
    ctx->gpr[0] = r0;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    ctx->cr = cr;
    InvokeIndirectCpu(target, ctx);
}

} // namespace

// d3d::rtexQuadManyTwin_c::drawTexQuadManyTwin (0x8000D000): picks the drawing by the object's kind
// (and, for magma, by the current world and level), instruction for instruction.
extern "C" void RtexQuadManyTwinDraw_HLE_8000d000(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    uint32_t cr = ctx->cr;
    const uint32_t xer = ctx->xer;
    uint32_t r4 = self + 0x10000u;
    const uint32_t r5 = MemoryInline::FlatRead32(ctx->gpr[13] - 21688u);
    static_assert(0x10000u - 12652u == kKind);
    uint32_t r0 = MemoryInline::FlatRead32(r4 - 12652u);  // the kind
    SetCRResident(cr, xer, 0, static_cast<int32_t>(r0), 0);
    if (r0 == 0) {
        ServiceShots();
        if (WaterHleEnabled()) {
            ctx->gpr[0] = r0;
            ctx->gpr[4] = r4;
            ctx->gpr[5] = r5;
            ctx->cr = cr;
            DrawNormalHle(ctx);
        } else {
            CallTranslated(ctx, kDrawNormal, r0, r4, r5, cr);
        }
        return;
    }
    if (r0 == 1) {
        r4 = MemoryInline::FlatRead8(r5 + 4620u);
        bool hi = false;
        if (r4 == 7) {
            r0 = MemoryInline::FlatRead8(r5 + 4621u);
            SetCRResident(cr, xer, 0, r0, 2u);
            hi = r0 == 2;
        }
        if (!hi && r4 == 7) {
            r0 = MemoryInline::FlatRead8(r5 + 4621u);
            if (r0 == 23) {
                r0 = MemoryInline::FlatRead8(r5 + 4622u);
                SetCRResident(cr, xer, 0, r0, 3u);
                hi = r0 == 3;
            }
        }
        if (!hi) {
            SetCRResident(cr, xer, 0, r4, 1u);
            if (r4 == 1) {
                r0 = MemoryInline::FlatRead8(r5 + 4621u);
                SetCRResident(cr, xer, 0, r0, 23u);
                if (r0 == 23) {
                    r0 = MemoryInline::FlatRead8(r5 + 4622u);
                    SetCRResident(cr, xer, 0, static_cast<int32_t>(r0), 0);
                    hi = r0 == 0;
                }
            }
        }
        CallTranslated(ctx, hi ? kDrawMagmaHi : kDrawMagma, r0, r4, r5, cr);
        return;
    }
    SetCRResident(cr, xer, 0, static_cast<int32_t>(r0), 2);
    if (r0 == 2) {
        CallTranslated(ctx, kDrawPoison, r0, r4, r5, cr);
        return;
    }
    SetCRResident(cr, xer, 0, static_cast<int32_t>(r0), 4);
    if (r0 == 4) {
        CallTranslated(ctx, kDrawSand, r0, r4, r5, cr);
    }
}
PPC_NATIVE_OVERRIDE_VOID(8000D000, RtexQuadManyTwinDraw_HLE_8000d000, (CpuContext* ctx), (ctx));
