#include "gx_internal.h"
#include "gx_stream_common.h"
#include "gx_cp_decode.h"
#include "isa/big_endian.h"

#include <algorithm>

// Opcode constants and the stream helpers this file shares with gx_dl.cpp /
// gx_vertex.cpp; see gx_stream_common.h.
using namespace GxCmd;
using namespace GxStream;

HleGxState g_hleGxState;
namespace GxStream { uint64_t g_gxVtxPublishCount = 0; }
uint64_t g_gxBeginCount = 0;
uint64_t g_gxBeginRawCount = 0;

namespace aurora::gx::fifo {
bool submit_raw_draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount,
                     uint32_t vertexBytes);
}

void HleGxState::ResetVertex() {
    // NSMBW_GXFAST: the first enabled attribute only changes with the descriptor, and every
    // descriptor change bumps vtxStateGeneration (each GXBegin and raw draw calls this, some twice).
    if (GxFast() && this == &g_hleGxState) {
        static bool valid = false;
        static uint32_t generation = 0;
        static GXAttr first = GX_VA_NULL;
        if (!valid || generation != vtxStateGeneration) {
            first = NextEnabledAttr(GX_VA_PNMTXIDX - 1);
            generation = vtxStateGeneration;
            valid = true;
        }
        currentAttr = first;
        currentComp = 0;
        return;
    }
    currentAttr = NextEnabledAttr(GX_VA_PNMTXIDX - 1);
    currentComp = 0;
}

GXAttr HleGxState::NextEnabledAttr(int startAttr) {
    for (int i = startAttr + 1; i < 26; ++i) {
        if (vtxDesc[i] != GX_NONE) {
            return static_cast<GXAttr>(i);
        }
    }
    return GX_VA_NULL;
}

int HleGxState::GetExpectedCompCount(GXAttr attr, const VtxAttrFmt& fmt) {
    // Attributes with no component layout (matrix indices, XF arrays, GX_VA_NBT)
    // count as one component here: the incremental parser advances one element
    // per raw stream item for them.
    return static_cast<int>(AttrCompCount(attr, fmt, 1u));
}

// NSMBW_GXFAST_LOG=1 (diagnostics): every 65536 raw draws, how often the republish was skipped and
// why it was not (first use, HLE state changed, aurora's shadow changed and in which registers, or
// AuroraResendVtxShadow refused: no GX thread).
static bool RawPublishLogOn() {
    static const bool on = std::getenv("NSMBW_GXFAST_LOG") != nullptr;
    return on;
}

static void NoteRawPublish(int reason, uint32_t differs) {
    if (!RawPublishLogOn()) {
        return;
    }
    static uint64_t counts[5]{};
    static uint64_t wordMiss[7]{};
    ++counts[reason];
    for (int i = 0; i < 7; ++i) wordMiss[i] += (differs >> i) & 1u;
    const uint64_t total = counts[0] + counts[1] + counts[2] + counts[3] + counts[4];
    if ((total & 0xFFFFu) == 0) {
        std::fprintf(stderr,
                     "[gxfast] raw draws %llu: skipped %llu, published: first %llu, hle changed %llu, "
                     "shadow changed %llu (vcdLo %llu vcdHi %llu vatA %llu vatB %llu vatC %llu nrmType %llu "
                     "nrms %llu), refused %llu\n",
                     (unsigned long long)total, (unsigned long long)counts[0], (unsigned long long)counts[1],
                     (unsigned long long)counts[2], (unsigned long long)counts[3],
                     (unsigned long long)wordMiss[0], (unsigned long long)wordMiss[1],
                     (unsigned long long)wordMiss[2], (unsigned long long)wordMiss[3],
                     (unsigned long long)wordMiss[4], (unsigned long long)wordMiss[5],
                     (unsigned long long)wordMiss[6], (unsigned long long)counts[4]);
    }
}

static void ApplyAuroraVtxStateForRawBegin(GXVtxFmt fmt) {
    // KNOWN DIVERGENCE: this is the only publish site with includeNbt=true, so it also
    // publishes GX_VA_NBT after GX_VA_NRM, which makes aurora's SETVAT fall-through clobber the
    // NRM VAT with the default. Raw-FIFO geometry renders correctly with it; do not "fix" without
    // an in-race A/B run.
    constexpr AuroraVtxPublishOptions kOptions{/*includeNbt=*/true, /*fmtLoopFirst=*/0, /*fmtLoopLast=*/25};
    if (!GxFast() || fmt >= GX_MAX_VTXFMT) {
        PublishAuroraVtxState(fmt, kOptions);
        return;
    }
    // NSMBW_GXFAST: the publish is a pure function of the HLE descriptor and format row (unchanged
    // while vtxStateGeneration is) and of aurora's shadow registers, and applying it twice changes
    // nothing. So when the HLE state is the one last published for this format and aurora's shadow
    // is still exactly what that publish left, the 46 setter calls would only mark VCD/VAT dirty
    // (GX thread: aurora cannot compare against decoded state), which AuroraResendVtxShadow does
    // directly; the FIFO then gets the same re-sent registers as before. Anything else that went
    // through aurora's setters in between (the other publish sites, GXClearVtxDesc, ...) shows up as
    // a changed shadow; lists that change decoded state behind the shadow are covered because the
    // registers are still re-sent on every draw.
    struct Published {
        bool valid = false;
        uint32_t generation = 0;
        uint32_t shadow[7]{};
    };
    static Published published[GX_MAX_VTXFMT];
    Published& last = published[fmt];
    if (last.valid && last.generation == g_hleGxState.vtxStateGeneration &&
        AuroraResendVtxShadow(fmt, last.shadow)) {
        NoteRawPublish(0, 0);
        return;
    }
    if (RawPublishLogOn()) {
        uint32_t now[7];
        AuroraGetVtxShadow(fmt, now);
        uint32_t differs = 0;
        for (int i = 0; i < 7; ++i) differs |= (now[i] != last.shadow[i] ? 1u : 0u) << i;
        NoteRawPublish(!last.valid ? 1 : last.generation != g_hleGxState.vtxStateGeneration ? 2 : differs ? 3 : 4,
                       differs);
    }
    PublishAuroraVtxState(fmt, kOptions);
    last.valid = true;
    last.generation = g_hleGxState.vtxStateGeneration;
    AuroraGetVtxShadow(fmt, last.shadow);
}

// There is deliberately no indexed-aurora path here. Immediate-mode indexed
// draws are parsed incrementally, and aurora's indexed-array upload has to
// precede the draw carrying the max-index bounds, which cannot be known without
// buffering the whole primitive. Indexed attributes are therefore expanded into
// the packed direct stream above (GX_INDEX8/16 -> GX_DIRECT).

static uint32_t GetDirectAttrByteSizeForFifo(GXAttr attr, const VtxAttrFmt& fmt) {
    return DirectAttrByteSize(attr, fmt, /*matrixAttrIsOneByte=*/true, /*fallbackComps=*/1u);
}

static bool ComputeRawDirectFifoVertexSize(GXVtxFmt fmt, uint32_t& vertexSize) {
    vertexSize = 0;
    if (fmt >= GX_MAX_VTXFMT) {
        return false;
    }

    for (int attr = 0; attr < 26; ++attr) {
        const GXAttrType type = g_hleGxState.vtxDesc[attr];
        if (type == GX_NONE) {
            continue;
        }
        if (type != GX_DIRECT) {
            return false;
        }

        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        const uint32_t attrBytes =
            GetDirectAttrByteSizeForFifo(gxAttr, g_hleGxState.vtxAttrFmt[fmt][attr]);
        if (attrBytes == 0) {
            return false;
        }
        vertexSize += attrBytes;
    }

    return vertexSize != 0;
}

// NSMBW_GXFAST: remembered per format until the next descriptor or format change (every one bumps
// vtxStateGeneration), instead of walking all 26 attributes on every GXBegin and FIFO draw.
static bool TryGetRawDirectFifoVertexSize(GXVtxFmt fmt, uint32_t& vertexSize) {
    if (!GxFast() || fmt >= GX_MAX_VTXFMT) {
        return ComputeRawDirectFifoVertexSize(fmt, vertexSize);
    }
    struct Entry {
        bool valid = false;
        bool ok = false;
        uint32_t generation = 0;
        uint32_t size = 0;
    };
    static Entry cache[GX_MAX_VTXFMT];
    Entry& entry = cache[fmt];
    if (!entry.valid || entry.generation != g_hleGxState.vtxStateGeneration) {
        entry.ok = ComputeRawDirectFifoVertexSize(fmt, entry.size);
        entry.generation = g_hleGxState.vtxStateGeneration;
        entry.valid = true;
    }
    vertexSize = entry.size;
    return entry.ok;
}

// NSMBW_RAWINDEX (default on with GXFAST, =0 off): a GXBegin whose format is direct except for
// indexed positions, texture coordinates or RGB8/RGBA8 colours also goes the raw way. Its bytes are
// gathered as the guest writes them (indices included) and, once complete, each index is replaced
// by the array element it points at. That is exactly what the per-attribute path hands aurora
// (SubmitIndexedAttribute: GXPosition/GXTexCoord/GXColor of the element's raw components, which
// aurora encodes back into the same bytes), and aurora's descriptor already says GX_DIRECT for
// them (PublishAuroraVtxState). nw4r::ef billboards (snowfall on world map 3: direct F32 position,
// GX_INDEX8 U8 texture coordinate into a four-corner table) are the bulk of such draws.
static bool RawIndexOn() {
    static const bool on = [] {
        const char* v = std::getenv("NSMBW_RAWINDEX");
        const bool enabled = v == nullptr || v[0] != '0';
        std::fprintf(stderr, "[gx] NSMBW_RAWINDEX=%d\n", enabled ? 1 : 0);
        return enabled;
    }();
    return on;
}

struct RawIndexLayout {
    struct Step {
        uint8_t attr;
        uint8_t inBytes;   // in the guest's stream: the element itself, or a 1/2-byte index
        uint8_t outBytes;  // in aurora's direct stream
        uint8_t index;     // 0 direct, 1 GX_INDEX8, 2 GX_INDEX16
    };
    bool valid = false;
    bool ok = false;
    uint32_t generation = 0;
    uint32_t inSize = 0;
    uint32_t outSize = 0;
    uint32_t steps = 0;
    Step step[26]{};
};

static bool RawIndexAttrAllowed(int attr, const VtxAttrFmt& fmt) {
    if (attr == GX_VA_POS || (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7)) {
        return true;
    }
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
        return fmt.type == GX_RGB8 || fmt.type == GX_RGBA8;
    }
    return false;
}

static bool ComputeRawIndexLayout(GXVtxFmt fmt, RawIndexLayout& layout) {
    layout.inSize = 0;
    layout.outSize = 0;
    layout.steps = 0;
    bool anyIndexed = false;
    for (int attr = 0; attr < 26; ++attr) {
        const GXAttrType type = g_hleGxState.vtxDesc[attr];
        if (type == GX_NONE) {
            continue;
        }
        const VtxAttrFmt& f = g_hleGxState.vtxAttrFmt[fmt][attr];
        const uint32_t direct = GetDirectAttrByteSizeForFifo(static_cast<GXAttr>(attr), f);
        if (direct == 0 || direct > 0xFFu) {
            return false;
        }
        RawIndexLayout::Step& s = layout.step[layout.steps++];
        s.attr = static_cast<uint8_t>(attr);
        s.outBytes = static_cast<uint8_t>(direct);
        if (type == GX_DIRECT) {
            s.inBytes = s.outBytes;
            s.index = 0;
        } else if ((type == GX_INDEX8 || type == GX_INDEX16) && RawIndexAttrAllowed(attr, f)) {
            s.inBytes = type == GX_INDEX8 ? 1 : 2;
            s.index = type == GX_INDEX8 ? 1 : 2;
            anyIndexed = true;
        } else {
            return false;
        }
        layout.inSize += s.inBytes;
        layout.outSize += s.outBytes;
    }
    return anyIndexed && layout.outSize != 0;
}

static const RawIndexLayout* TryGetRawIndexLayout(GXVtxFmt fmt) {
    if (fmt >= GX_MAX_VTXFMT) {
        return nullptr;
    }
    static RawIndexLayout cache[GX_MAX_VTXFMT];
    RawIndexLayout& layout = cache[fmt];
    if (!layout.valid || layout.generation != g_hleGxState.vtxStateGeneration) {
        layout.ok = ComputeRawIndexLayout(fmt, layout);
        layout.generation = g_hleGxState.vtxStateGeneration;
        layout.valid = true;
    }
    return layout.ok ? &layout : nullptr;
}

// The open raw GXBegin gathers indices (HleTryBeginRaw); RawBeginFinish expands them.
static const RawIndexLayout* s_rawIndexLayout = nullptr;
static std::vector<uint8_t> s_rawIndexExpanded;

// Replaces every index of the gathered packet by its array element; false (nothing drawn) when an
// array is unset or an element lies outside guest memory, so the caller takes the per-attribute
// path for exactly what it would have done there.
static bool ExpandRawIndexedPacket(const RawIndexLayout& layout, const uint8_t* in, uint16_t verts,
                                   std::vector<uint8_t>& out) {
    out.resize(3u + static_cast<size_t>(verts) * layout.outSize);
    std::memcpy(out.data(), in, 3);
    uint8_t* o = out.data() + 3;
    const uint8_t* p = in + 3;
    for (uint32_t v = 0; v < verts; ++v) {
        for (uint32_t i = 0; i < layout.steps; ++i) {
            const RawIndexLayout::Step& s = layout.step[i];
            if (s.index == 0) {
                std::memcpy(o, p, s.inBytes);
            } else {
                const uint32_t index = s.index == 1 ? p[0] : ReadBE16(p);
                const auto& arr = g_hleGxState.vtxArray[s.attr];
                if (arr.base == 0 || arr.stride == 0) {
                    return false;
                }
                const uint32_t addr = arr.base + index * arr.stride;
                if (!Memory::Contains(addr, s.outBytes)) {
                    return false;
                }
                const auto* src = static_cast<const uint8_t*>(GuestToHostPtr(addr, s.outBytes));
                if (src == nullptr) {
                    return false;
                }
                std::memcpy(o, src, s.outBytes);
            }
            p += s.inBytes;
            o += s.outBytes;
        }
    }
    return true;
}

static bool TrySubmitRawDirectFifoDraw(const uint8_t* packet, uint32_t packetBytes, GXPrimitive prim,
                                       GXVtxFmt vtxFmt, uint16_t vtxCount) {
    if (packet == nullptr || packetBytes == 0 || vtxCount == 0) {
        return false;
    }

    EnsureAuroraFrameActive();

    g_hleGxState.currentVtxFmt = vtxFmt;
    g_hleGxState.currentPrim = prim;
    g_hleGxState.vertsRemaining = vtxCount;
    g_hleGxState.inBegin = false;
    g_hleGxState.auroraBeginCalled = false;
    g_hleGxState.ResetVertex();

    ApplyAuroraVtxStateForRawBegin(vtxFmt);
    EnsureDefaultGxAlphaCompare();

    if (!aurora::gx::fifo::submit_raw_draw(prim, vtxFmt, packet + 3, vtxCount, packetBytes - 3u)) {
        return false;
    }
    GXMarkFrameWork();
    return true;
}

static void HleFifoWriteBurstChunked(const uint8_t* data, uint32_t sizeBytes);

// NSMBW: GXBegin with an all-direct vertex format (its tiles, text glyphs, layout and lighting
// quads). The per-attribute path below decodes every component to a float, hands it to aurora's
// GXPosition/GXTexCoord/GXColor, which encode it back into the very same bytes, and aurora then
// decodes those again: three passes per component. Here the guest's bytes are gathered as they
// arrive and drawn raw once the declared vertices are all in, which aurora's shaders read as they
// are. NSMBW_NO_RAW_BEGIN=1 keeps the per-attribute path (A/B).
bool HleTryBeginRaw(GXPrimitive prim, GXVtxFmt fmt, uint16_t verts) {
    static const bool off = std::getenv("NSMBW_NO_RAW_BEGIN") != nullptr;
    uint32_t vertexSize = 0;
    s_rawIndexLayout = nullptr;
    if (!off && verts != 0 && GxFast() && RawIndexOn() && !TryGetRawDirectFifoVertexSize(fmt, vertexSize)) {
        s_rawIndexLayout = TryGetRawIndexLayout(fmt);
        if (s_rawIndexLayout != nullptr) {
            vertexSize = s_rawIndexLayout->inSize;
        }
    }
    if (off || verts == 0 || (s_rawIndexLayout == nullptr && !TryGetRawDirectFifoVertexSize(fmt, vertexSize))) {
        if (RawPublishLogOn() && !off && verts != 0) {
            // NSMBW_GXFAST_LOG: the first descriptors that kept a GXBegin off the raw path.
            static std::vector<uint64_t> seen;
            uint64_t key = fmt;
            for (int a = 0; a < 21; ++a) key = key * 3u + (g_hleGxState.vtxDesc[a] == GX_NONE ? 0u : g_hleGxState.vtxDesc[a] == GX_DIRECT ? 1u : 2u);
            if (seen.size() < 16 && std::find(seen.begin(), seen.end(), key) == seen.end()) {
                seen.push_back(key);
                std::fprintf(stderr, "[gxfast] per-attribute GXBegin fmt %d prim 0x%02X verts %u, desc:", fmt, prim, verts);
                for (int a = 0; a < 26; ++a) {
                    if (g_hleGxState.vtxDesc[a] == GX_NONE) continue;
                    const auto& f = g_hleGxState.vtxAttrFmt[fmt][a];
                    std::fprintf(stderr, " %d=%d(cnt %d type %d frac %d)", a, g_hleGxState.vtxDesc[a], f.cnt, f.type, f.frac);
                }
                std::fprintf(stderr, "\n");
            }
        }
        return false;
    }
    auto& s = g_hleGxState;
    s.rawBegin = true;
    s.rawExpected = 3u + static_cast<uint32_t>(verts) * vertexSize;
    if (s.rawPacket.size() < s.rawExpected) {
        s.rawPacket.resize(s.rawExpected);
    }
    s.rawPacket[0] = static_cast<uint8_t>(fmt | prim);
    s.rawPacket[1] = static_cast<uint8_t>(verts >> 8);
    s.rawPacket[2] = static_cast<uint8_t>(verts);
    s.rawFilled = 3;
    return true;
}

void HleEndRaw() {
    g_hleGxState.rawBegin = false;
    s_rawIndexLayout = nullptr;
}

static void RawBeginFinish() {
    auto& s = g_hleGxState;
    s.rawBegin = false;
    const GXPrimitive prim = s.currentPrim;
    const GXVtxFmt fmt = s.currentVtxFmt;
    const auto verts = static_cast<uint16_t>(s.vertsRemaining);
    const RawIndexLayout* indexed = s_rawIndexLayout;
    s_rawIndexLayout = nullptr;
    if (indexed != nullptr) {
        if (ExpandRawIndexedPacket(*indexed, s.rawPacket.data(), verts, s_rawIndexExpanded) &&
            TrySubmitRawDirectFifoDraw(s_rawIndexExpanded.data(), static_cast<uint32_t>(s_rawIndexExpanded.size()),
                                       prim, fmt, verts)) {
            return;
        }
    } else if (TrySubmitRawDirectFifoDraw(s.rawPacket.data(), s.rawExpected, prim, fmt, verts)) {
        return;
    }
    // Aurora refused it: run the same bytes through the per-attribute path.
    s.currentVtxFmt = fmt;
    s.currentPrim = prim;
    s.vertsRemaining = verts;
    s.inBegin = true;
    s.auroraBeginCalled = false;
    s.fifoReadOffset = 0;
    s.fifoByteCount = 0;
    s.ResetVertex();
    const std::vector<uint8_t> vertices(s.rawPacket.begin() + 3, s.rawPacket.begin() + s.rawExpected);
    HleFifoWriteBurstChunked(vertices.data(), static_cast<uint32_t>(vertices.size()));
}

// Takes what the open raw GXBegin still needs of `data`, drawing once complete; returns the bytes taken.
static uint32_t RawBeginAppend(const uint8_t* data, uint32_t size) {
    auto& s = g_hleGxState;
    const uint32_t take = std::min(s.rawExpected - s.rawFilled, size);
    std::memcpy(s.rawPacket.data() + s.rawFilled, data, take);
    s.rawFilled += take;
    if (s.rawFilled == s.rawExpected) {
        RawBeginFinish();
    }
    return take;
}

static void DecodeColorFromArray(uint32_t addr, GXCompType type, GXCompCnt cnt, GXColor& out) {
    // Gather exactly the bytes the shared decoder consumes. GX_RGBX8 is the one
    // format whose stream footprint (4 bytes) exceeds what is decoded (3), and
    // the pre-dedup code read only those 3 from the guest array - keep it that
    // way so a 3-byte tail at the end of an array cannot start faulting.
    const uint32_t byteCount =
        (type == GX_RGBX8) ? 3u : ColorByteSize(type, cnt);
    uint8_t bytes[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < byteCount && i < 4u; ++i) {
        bytes[i] = Memory::Read8(addr + i);
    }
    DecodeColorBytes(bytes, type, cnt, out);
}

void SubmitIndexedAttribute(GXAttr attr, uint32_t index) {
    const auto& fmt = g_hleGxState.vtxAttrFmt[g_hleGxState.currentVtxFmt][attr];
    const auto& arr = g_hleGxState.vtxArray[attr];
    if (arr.base == 0 || arr.stride == 0) {
        float comps[9]{};
        switch (attr) {
        case GX_VA_CLR0:
        case GX_VA_CLR1:
            if (fmt.cnt == GX_CLR_RGB) {
                GXColor3u8(0, 0, 0);
            } else {
                GXColor4u8(0, 0, 0, 0xFF);
            }
            break;
        case GX_VA_POS:
        case GX_VA_NRM:
        case GX_VA_TEX0:
        case GX_VA_TEX1:
        case GX_VA_TEX2:
        case GX_VA_TEX3:
        case GX_VA_TEX4:
        case GX_VA_TEX5:
        case GX_VA_TEX6:
        case GX_VA_TEX7:
            SubmitAttribute(attr, comps, fmt);
            break;
        default:
            break;
        }
        return;
    }
    const uint32_t baseAddr = arr.base + index * arr.stride;
    float comps[9]{};
    u32 rawComps[9]{};
    switch (attr) {
    case GX_VA_POS: {
        const int count = static_cast<int>(AttrCompCount(GX_VA_POS, fmt, 0u));
        const int step = GetCompSizeBytes(fmt.type);
        uint32_t addr = baseAddr;
        for (int i = 0; i < count; ++i, addr += step) {
            comps[i] = ReadArrayComp(addr, fmt.type, fmt.frac);
            rawComps[i] = ReadArrayRawComp(addr, fmt.type);
        }
        SubmitAttribute(attr, comps, fmt, rawComps);
        break;
    }
    case GX_VA_NRM: {
        const int count = static_cast<int>(AttrCompCount(GX_VA_NRM, fmt, 0u));
        const int step = GetCompSizeBytes(fmt.type);
        uint32_t addr = baseAddr;
        for (int i = 0; i < count; ++i, addr += step) {
            comps[i] = ReadArrayComp(addr, fmt.type, fmt.frac);
            rawComps[i] = ReadArrayRawComp(addr, fmt.type);
        }
        SubmitAttribute(attr, comps, fmt, rawComps);
        break;
    }
    case GX_VA_CLR0:
    case GX_VA_CLR1: {
        GXColor color{};
        DecodeColorFromArray(baseAddr, fmt.type, fmt.cnt, color);
        if (fmt.cnt == GX_CLR_RGB) {
            GXColor3u8(color.r, color.g, color.b);
        } else {
            GXColor4u8(color.r, color.g, color.b, color.a);
        }
        break;
    }
    case GX_VA_TEX0: case GX_VA_TEX1: case GX_VA_TEX2: case GX_VA_TEX3:
    case GX_VA_TEX4: case GX_VA_TEX5: case GX_VA_TEX6: case GX_VA_TEX7: {
        // Every GX_VA_TEXn shares one component layout, so the constant here is
        // exact for whichever of them `attr` is.
        const int count = static_cast<int>(AttrCompCount(GX_VA_TEX0, fmt, 0u));
        const int step = GetCompSizeBytes(fmt.type);
        uint32_t addr = baseAddr;
        for (int i = 0; i < count; ++i, addr += step) {
            comps[i] = ReadArrayComp(addr, fmt.type, fmt.frac);
            rawComps[i] = ReadArrayRawComp(addr, fmt.type);
        }
        SubmitAttribute(attr, comps, fmt, rawComps);
        break;
    }
    default:
        break;
    }
}

// Mirrors the raw-integer fast paths in SubmitAttribute below: for those
// (attr, type) pairs the float component buffer is never read, so the direct
// FIFO path can skip building it.
static bool SubmitAttributeReadsFloatComps(GXAttr attr, const VtxAttrFmt& fmt) {
    switch (attr) {
    case GX_VA_POS:
    case GX_VA_NRM:
    case GX_VA_TEX0: case GX_VA_TEX1: case GX_VA_TEX2: case GX_VA_TEX3:
    case GX_VA_TEX4: case GX_VA_TEX5: case GX_VA_TEX6: case GX_VA_TEX7:
        break;
    default:
        return true;
    }
    switch (fmt.type) {
    case GX_U8:
    case GX_S8:
    case GX_U16:
    case GX_S16:
        return false;
    default:
        return true;
    }
}

void SubmitAttribute(GXAttr attr, float* comps, const VtxAttrFmt& fmt, const u32* rawComps) {
    auto r8 = [rawComps](int idx) { return static_cast<u8>(rawComps[idx]); };
    auto rs8 = [rawComps](int idx) { return static_cast<s8>(rawComps[idx]); };
    auto r16 = [rawComps](int idx) { return static_cast<u16>(rawComps[idx]); };
    auto rs16 = [rawComps](int idx) { return static_cast<s16>(rawComps[idx]); };

    switch (attr) {
    case GX_VA_POS:
        if (rawComps) {
            if (fmt.cnt == GX_POS_XY) {
                switch (fmt.type) {
                case GX_U8: GXPosition2u8(r8(0), r8(1)); return;
                case GX_S8: GXPosition2s8(rs8(0), rs8(1)); return;
                case GX_U16: GXPosition2u16(r16(0), r16(1)); return;
                case GX_S16: GXPosition2s16(rs16(0), rs16(1)); return;
                default: break;
                }
            } else {
                switch (fmt.type) {
                case GX_U8: GXPosition3u8(r8(0), r8(1), r8(2)); return;
                case GX_S8: GXPosition3s8(rs8(0), rs8(1), rs8(2)); return;
                case GX_U16: GXPosition3u16(r16(0), r16(1), r16(2)); return;
                case GX_S16: GXPosition3s16(rs16(0), rs16(1), rs16(2)); return;
                default: break;
                }
            }
        }
        if (fmt.cnt == GX_POS_XY) {
            GXPosition2f32(comps[0], comps[1]);
        } else {
            GXPosition3f32(comps[0], comps[1], comps[2]);
        }
        break;
    case GX_VA_NRM:
        if (rawComps) {
            const bool nbt = fmt.cnt == GX_NRM_NBT || fmt.cnt == GX_NRM_NBT3;
            const int groups = nbt ? 3 : 1;
            switch (fmt.type) {
            case GX_U8:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3u8(r8(i * 3), r8(i * 3 + 1), r8(i * 3 + 2));
                }
                return;
            case GX_S8:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3s8(rs8(i * 3), rs8(i * 3 + 1), rs8(i * 3 + 2));
                }
                return;
            case GX_U16:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3u16(r16(i * 3), r16(i * 3 + 1), r16(i * 3 + 2));
                }
                return;
            case GX_S16:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3s16(rs16(i * 3), rs16(i * 3 + 1), rs16(i * 3 + 2));
                }
                return;
            default: break;
            }
        }
        if (fmt.cnt == GX_NRM_NBT || fmt.cnt == GX_NRM_NBT3) {
            for (int i = 0; i < 3; ++i) {
                GXNormal3f32(comps[i * 3], comps[i * 3 + 1], comps[i * 3 + 2]);
            }
        } else {
            GXNormal3f32(comps[0], comps[1], comps[2]);
        }
        break;
    case GX_VA_CLR0:
    case GX_VA_CLR1:
        if (fmt.cnt == GX_CLR_RGB) {
            GXColor3u8(static_cast<u8>(comps[0]), static_cast<u8>(comps[1]),
                       static_cast<u8>(comps[2]));
        } else {
            GXColor4u8(static_cast<u8>(comps[0]), static_cast<u8>(comps[1]),
                       static_cast<u8>(comps[2]), static_cast<u8>(comps[3]));
        }
        break;
    case GX_VA_TEX0: case GX_VA_TEX1: case GX_VA_TEX2: case GX_VA_TEX3:
    case GX_VA_TEX4: case GX_VA_TEX5: case GX_VA_TEX6: case GX_VA_TEX7:
        if (rawComps) {
            if (fmt.cnt == GX_TEX_S) {
                switch (fmt.type) {
                case GX_U8: GXTexCoord1u8(r8(0)); return;
                case GX_S8: GXTexCoord1s8(rs8(0)); return;
                case GX_U16: GXTexCoord1u16(r16(0)); return;
                case GX_S16: GXTexCoord1s16(rs16(0)); return;
                default: break;
                }
            } else {
                switch (fmt.type) {
                case GX_U8: GXTexCoord2u8(r8(0), r8(1)); return;
                case GX_S8: GXTexCoord2s8(rs8(0), rs8(1)); return;
                case GX_U16: GXTexCoord2u16(r16(0), r16(1)); return;
                case GX_S16: GXTexCoord2s16(rs16(0), rs16(1)); return;
                default: break;
                }
            }
        }
        if (fmt.cnt == GX_TEX_S) {
            GXTexCoord1f32(comps[0]);
        } else {
            GXTexCoord2f32(comps[0], comps[1]);
        }
        break;
    default: break;
    }
}

// `val` is a raw big-endian bit pattern: the FIFO stream is type-agnostic, and
// the float entry point converts before it gets here.
void HleFifoWrite(u32 val, uint32_t sizeBytes) {
    const bool recordOnly = IsDisplayListActive();
    if (recordOnly) {
        WriteDisplayListData(val, sizeBytes);
        return;
    }
    if (g_hleGxState.rawBegin) {
        const uint32_t count = sizeBytes == 0 ? 4u : std::min<uint32_t>(sizeBytes, 4u);
        auto& s = g_hleGxState;
        // NSMBW_GXFAST: a store that fits in the open draw goes straight into the packet (big-endian,
        // as below), not through a byte array and a library memcpy per float.
        if (GxFast() && s.rawExpected - s.rawFilled >= count) {
            uint8_t* dst = s.rawPacket.data() + s.rawFilled;
            if (count == 4u) {
                BigEndian::Write32(dst, val);
            } else if (count == 2u) {
                BigEndian::Write16(dst, static_cast<uint16_t>(val));
            } else {
                for (uint32_t i = 0; i < count; ++i) {
                    dst[i] = static_cast<uint8_t>(val >> (8u * (count - 1u - i)));
                }
            }
            s.rawFilled += count;
            if (s.rawFilled == s.rawExpected) {
                RawBeginFinish();
            }
            return;
        }
        uint8_t bytes[4];
        for (uint32_t i = 0; i < count; ++i) {
            bytes[i] = static_cast<uint8_t>(val >> (8u * (count - 1u - i)));
        }
        const uint32_t taken = RawBeginAppend(bytes, count);
        for (uint32_t i = taken; i < count; ++i) {
            HleFifoWrite(bytes[i], 1);  // past the last vertex: the next command's first bytes
        }
        return;
    }

    auto resetFifoBuffer = [&]() {
        g_hleGxState.fifoReadOffset = 0;
        g_hleGxState.fifoByteCount = 0;
    };

    auto compactFifoBuffer = [&]() {
        if (g_hleGxState.fifoByteCount == 0) {
            g_hleGxState.fifoReadOffset = 0;
            return;
        }
        if (g_hleGxState.fifoReadOffset == 0) {
            return;
        }
        std::memmove(g_hleGxState.fifoBytes.data(),
                     g_hleGxState.fifoBytes.data() + g_hleGxState.fifoReadOffset,
                     g_hleGxState.fifoByteCount);
        g_hleGxState.fifoReadOffset = 0;
    };

    auto fifoData = [&]() -> uint8_t* {
        return g_hleGxState.fifoBytes.data() + g_hleGxState.fifoReadOffset;
    };

    auto pushBytes = [&](u32 value, uint32_t count) {
        if (count == 0) return;
        if (count > 4) count = 4;
        if (g_hleGxState.fifoReadOffset + g_hleGxState.fifoByteCount + count > g_hleGxState.fifoBytes.size()) {
            compactFifoBuffer();
            if (g_hleGxState.fifoReadOffset + g_hleGxState.fifoByteCount + count > g_hleGxState.fifoBytes.size()) {
                resetFifoBuffer();
            }
        }
        const size_t writeOffset = g_hleGxState.fifoReadOffset + g_hleGxState.fifoByteCount;
        switch (count) {
        case 4:
            BigEndian::Write32(g_hleGxState.fifoBytes.data() + writeOffset, value);
            break;
        case 2:
            BigEndian::Write16(g_hleGxState.fifoBytes.data() + writeOffset,
                               static_cast<uint16_t>(value));
            break;
        default:
            g_hleGxState.fifoBytes[writeOffset] = static_cast<uint8_t>(value & 0xFF);
            break;
        }
        g_hleGxState.fifoByteCount += count;
    };

    auto consumeBytes = [&](uint32_t count, u32& out) -> bool {
        if (count == 0 || g_hleGxState.fifoByteCount < count) return false;
        u32 value = 0;
        const uint32_t foldCount = (count > 4) ? 4 : count;
        uint8_t* data = fifoData();
        for (uint32_t i = 0; i < foldCount; ++i) {
            value = (value << 8) | data[i];
        }
        g_hleGxState.fifoReadOffset += count;
        g_hleGxState.fifoByteCount -= count;
        if (g_hleGxState.fifoByteCount == 0) {
            g_hleGxState.fifoReadOffset = 0;
        }
        out = value;
        return true;
    };

    pushBytes(val, sizeBytes == 0 ? 4 : sizeBytes);

    // Parse raw FIFO command packets written directly to the gather pipe
    // (e.g. NW4R/G3D paths that do not call the GXBegin wrapper function).
    while (!g_hleGxState.inBegin) {
        if (g_hleGxState.fifoByteCount < 1) {
            break;
        }

        uint8_t* data = fifoData();
        const uint8_t cmd = data[0];
        const uint8_t opcode = cmd & GX_OPCODE_MASK_CMD;
        u32 sink = 0;

        if (cmd == GX_NOP_CMD || opcode == GX_CMD_INVL_VC_CMD) {
            if (!consumeBytes(1, sink)) break;
            continue;
        }

        if (cmd == GX_LOAD_BP_REG_CMD) {
            if (g_hleGxState.fifoByteCount < 5) break;
            const uint32_t bpWord = ReadBE32(data + 1);
            GXApplyBPReg(static_cast<uint8_t>(bpWord >> 24), bpWord & 0x00FFFFFFu);
            if (!consumeBytes(5, sink)) break;
            continue;
        }

        if (opcode == GX_LOAD_CP_REG_CMD) {
            if (g_hleGxState.fifoByteCount < 6) break;
            const uint8_t reg = data[1];
            const uint32_t cpValue = ReadBE32(data + 2);
            GxCpDecode::ApplyCpRegWrite(reg, cpValue);
            if (!consumeBytes(6, sink)) break;
            continue;
        }

        if (opcode == GX_LOAD_XF_REG_CMD) {
            if (g_hleGxState.fifoByteCount < 5) break;
            const uint16_t countWords = ReadBE16(data + 1);
            const uint32_t packetBytes = 1u + 4u + (static_cast<uint32_t>(countWords) + 1u) * 4u;
            if (g_hleGxState.fifoByteCount < packetBytes) break;
            GXCallDisplayList(data, packetBytes);
            GXMarkFrameWork();
            if (!consumeBytes(packetBytes, sink)) break;
            continue;
        }

        if (opcode >= GX_LOAD_INDX_A_CMD && opcode <= GX_LOAD_INDX_D_CMD) {
            if (g_hleGxState.fifoByteCount < 5) break;
            const uint32_t xfValue = ReadBE32(data + 1);
            ApplyIndexedXfArrayForPacket(cmd, xfValue);
            GXCallDisplayList(data, 5);
            GXMarkFrameWork();
            if (!consumeBytes(5, sink)) break;
            continue;
        }

        if (opcode == GX_CMD_CALL_DL_CMD) {
            if (g_hleGxState.fifoByteCount < 9) break;
            const uint32_t listAddr = ReadBE32(data + 1);
            const uint32_t listSize = ReadBE32(data + 5);
            if (!consumeBytes(9, sink)) break;
            if (listAddr != 0 && listSize > 0) {
                GX__CallDisplayList_80172f64(listAddr, listSize);
            }
            continue;
        }

        if (IsDrawOpcode(opcode)) {
            if (g_hleGxState.fifoByteCount < 3) break;
            const uint16_t vtxCount = ReadBE16(data + 1);
            const GXVtxFmt vtxFmt = static_cast<GXVtxFmt>(cmd & GX_VAT_MASK_CMD);
            const GXPrimitive prim = OpcodeToGXPrimitive(cmd);
            uint32_t rawVertexSize = 0;
            if (TryGetRawDirectFifoVertexSize(vtxFmt, rawVertexSize)) {
                const uint32_t packetBytes = 3u + static_cast<uint32_t>(vtxCount) * rawVertexSize;
                if (g_hleGxState.fifoByteCount < packetBytes) {
                    break;
                }
                if (TrySubmitRawDirectFifoDraw(data, packetBytes, prim, vtxFmt, vtxCount)) {
                    if (!consumeBytes(packetBytes, sink)) break;
                    continue;
                }
            }
            if (!consumeBytes(3, sink)) break;

            g_hleGxState.currentVtxFmt = vtxFmt;
            g_hleGxState.currentPrim = prim;
            g_hleGxState.vertsRemaining = vtxCount;
            g_hleGxState.inBegin = true;
            g_hleGxState.auroraBeginCalled = false;
            g_hleGxState.ResetVertex();

            break;
        }

        // Unknown FIFO command byte outside a begin packet; discard it so stream parsing can recover.
        if (!consumeBytes(1, sink)) break;
    }

    if (!g_hleGxState.inBegin) {
        return;
    }

    if (!recordOnly && !g_hleGxState.auroraBeginCalled) {
        EnsureAuroraFrameActive();
        ApplyAuroraVtxStateForRawBegin(g_hleGxState.currentVtxFmt);
        EnsureDefaultGxAlphaCompare();
        GXBegin(g_hleGxState.currentPrim, g_hleGxState.currentVtxFmt, static_cast<u16>(g_hleGxState.vertsRemaining));
        g_hleGxState.auroraBeginCalled = true;
        GXMarkFrameWork();
    }

    auto finishVertexIfNeeded = [&](GXAttr prevAttr) {
        g_hleGxState.currentAttr = g_hleGxState.NextEnabledAttr(prevAttr);
        g_hleGxState.currentComp = 0;
        if (g_hleGxState.currentAttr == GX_VA_NULL) {
            g_hleGxState.ResetVertex();
            if (g_hleGxState.vertsRemaining > 0) {
                g_hleGxState.vertsRemaining--;
                if (g_hleGxState.vertsRemaining == 0) {
                    g_hleGxState.inBegin = false;
                    g_hleGxState.auroraBeginCalled = false;
                    if (!recordOnly) {
                        GXEnd();
                    }
                }
            }
        }
    };

    while (true) {
        if (!g_hleGxState.inBegin) {
            break;
        }
        GXAttr attr = g_hleGxState.currentAttr;
        if (attr == GX_VA_NULL) {
            resetFifoBuffer();
            return;
        }

        GXAttrType inputType = g_hleGxState.vtxDesc[attr];
        const VtxAttrFmt& fmt = g_hleGxState.vtxAttrFmt[g_hleGxState.currentVtxFmt][attr];

        if (IsMatrixIndexAttr(attr)) {
            if (g_hleGxState.fifoByteCount < 1) break;
            u32 raw = 0;
            if (!consumeBytes(1, raw)) break;
            if (!recordOnly) {
                GXMatrixIndex1u8(attr, static_cast<u8>(raw));
            }
            finishVertexIfNeeded(attr);
            continue;
        }

        if (inputType == GX_DIRECT) {
            if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
                const uint32_t colorSize = ColorByteSize(fmt.type, fmt.cnt);

                if (g_hleGxState.fifoByteCount < colorSize) break;

                uint8_t colorBytes[4] = {0, 0, 0, 0};
                uint8_t* data = fifoData();
                for (uint32_t i = 0; i < colorSize && i < 4; ++i) {
                    colorBytes[i] = data[i];
                }
                g_hleGxState.fifoReadOffset += colorSize;
                g_hleGxState.fifoByteCount -= colorSize;
                if (g_hleGxState.fifoByteCount == 0) {
                    g_hleGxState.fifoReadOffset = 0;
                }

                GXColor color{};
                DecodeColorBytes(colorBytes, fmt.type, fmt.cnt, color);

                if (!recordOnly) {
                    if (fmt.cnt == GX_CLR_RGB) {
                        GXColor3u8(color.r, color.g, color.b);
                    } else {
                        GXColor4u8(color.r, color.g, color.b, color.a);
                    }
                }
                finishVertexIfNeeded(attr);
                continue;
            }
            
            const int compSize = GetCompSizeBytes(fmt.type);
            if (compSize <= 0 || g_hleGxState.fifoByteCount < static_cast<size_t>(compSize)) break;
            u32 raw = 0;
            if (!consumeBytes(static_cast<uint32_t>(compSize), raw)) break;
            constexpr int kMaxComps = 9;
            if (g_hleGxState.currentComp < kMaxComps) {
                if (SubmitAttributeReadsFloatComps(attr, fmt)) {
                    g_hleGxState.compBuffer[g_hleGxState.currentComp] =
                        ConvertCompToFloat(raw, fmt.type, fmt.frac);
                }
                g_hleGxState.rawCompBuffer[g_hleGxState.currentComp] = raw;
            }
            g_hleGxState.currentComp++;
            const int expected = g_hleGxState.GetExpectedCompCount(attr, fmt);
            if (g_hleGxState.currentComp >= expected) {
                if (!recordOnly) {
                    SubmitAttribute(attr, g_hleGxState.compBuffer, fmt, g_hleGxState.rawCompBuffer);
                }
                finishVertexIfNeeded(attr);
            }
            continue;
        }

        if (inputType == GX_INDEX8 || inputType == GX_INDEX16) {
            const uint32_t idxSize = (inputType == GX_INDEX8) ? 1u : 2u;
            const uint32_t indexCount = (attr == GX_VA_NRM) ? NormalIndexCount(fmt) : 1u;
            if (g_hleGxState.fifoByteCount < idxSize * indexCount) break;
            u32 raw[3]{};
            for (uint32_t i = 0; i < indexCount; ++i) {
                if (!consumeBytes(idxSize, raw[i])) break;
            }
            if (!recordOnly) {
                if (attr == GX_VA_NRM && indexCount == 3u) {
                    SubmitIndexedNormalNBT3(raw, fmt);
                } else {
                    SubmitIndexedAttribute(attr, raw[0]);
                }
            }
            finishVertexIfNeeded(attr);
            continue;
        }

        g_hleGxState.currentAttr = g_hleGxState.NextEnabledAttr(attr);
        g_hleGxState.currentComp = 0;
        if (g_hleGxState.currentAttr == GX_VA_NULL) {
            g_hleGxState.ResetVertex();
        }
        break;
    }
}

// Must behave exactly like GX_HLE_FIFO_Write32/16/8 run byte by byte. The ring is not
// bulk-appended ahead of a single parse because the parser can re-enter GX HLE (nested display
// lists, XF/draw calls), which would misorder that work relative to the stream.
static void HleFifoWriteBurstChunked(const uint8_t* data, uint32_t sizeBytes) {
    uint32_t offset = 0;
    for (; offset + 4u <= sizeBytes; offset += 4u) {
        HleFifoWrite(ReadBE32(data + offset), 4);
    }
    if (offset + 2u <= sizeBytes) {
        HleFifoWrite(static_cast<u32>(ReadBE16(data + offset)), 2);
        offset += 2u;
    }
    if (offset < sizeBytes) {
        HleFifoWrite(static_cast<u32>(data[offset]), 1);
    }
}

// Applies complete register-load packets straight to the parser's own entry points, skipping the
// serialize/ring/re-parse round trip; returns bytes consumed, caller hands the rest to
// HleFifoWriteBurstChunked. Safe (per gd_fifo_hle.cpp's GdCanApplyDirect) only on a packet
// boundary with nothing buffered, outside a GXBegin packet and outside display-list recording;
// any other opcode or a truncated packet just stops the walk.
static uint32_t ApplyFifoPacketsDirect(const uint8_t* data, uint32_t sizeBytes) {
    uint32_t offset = 0;

    while (offset < sizeBytes) {
        // Re-tested per packet, not once per burst: nothing currently re-enters GX HLE mid-walk,
        // but if it ever does, breaking here just hands the remainder to the ring.
        if (IsDisplayListActive() || g_hleGxState.inBegin || g_hleGxState.fifoByteCount != 0) {
            break;
        }

        const uint8_t* packet = data + offset;
        const uint32_t avail = sizeBytes - offset;
        const uint8_t cmd = packet[0];
        const uint8_t opcode = cmd & GX_OPCODE_MASK_CMD;

        if (cmd == GX_NOP_CMD) {
            offset += 1u;
            continue;
        }

        if (cmd == GX_LOAD_BP_REG_CMD) {
            if (avail < 5u) break;
            const uint32_t bpWord = ReadBE32(packet + 1);
            GXApplyBPReg(static_cast<uint8_t>(bpWord >> 24), bpWord & 0x00FFFFFFu);
            offset += 5u;
            continue;
        }

        if (opcode == GX_LOAD_CP_REG_CMD) {
            if (avail < 6u) break;
            const uint8_t reg = packet[1];
            const uint32_t cpValue = ReadBE32(packet + 2);
            // Same function the parser calls, so the CP registers it does not
            // decode (0x30/0x40 among them) are dropped here identically.
            GxCpDecode::ApplyCpRegWrite(reg, cpValue);
            offset += 6u;
            continue;
        }

        if (opcode == GX_LOAD_XF_REG_CMD) {
            if (avail < 5u) break;
            const uint16_t countWords = ReadBE16(packet + 1);
            const uint32_t packetBytes = 1u + 4u + (static_cast<uint32_t>(countWords) + 1u) * 4u;
            if (avail < packetBytes) break;
            GXCallDisplayList(packet, packetBytes);
            GXMarkFrameWork();
            offset += packetBytes;
            continue;
        }

        break;
    }

    return offset;
}

// Display-list recording just copies bytes into the guest list buffer and advances the shadow
// cursor, so a burst is one logical append as long as it doesn't cross the list end; a burst that
// would wrap falls back to the per-write path instead. Returns false to signal that fallback.
static bool WriteDisplayListBurst(const uint8_t* data, uint32_t sizeBytes) {
    GxDisplayListState& dl = g_dlRecordState;
    if (!dl.active || dl.base == 0 || dl.size == 0 || dl.writePtr == 0) {
        // Same guard WriteDisplayListData applies; let the per-write path
        // reproduce its (silent) drop.
        return false;
    }

    const uint32_t end = dl.base + dl.size;
    if (dl.writePtr > end || (end - dl.writePtr) < sizeBytes) {
        return false;
    }

    // Guest-visible bytes are written through the ordinary store path, so
    // unaligned cursors, MMIO policy and executable-write guards all behave as
    // they do for a single store. The cursor and count advance per element so a
    // failed write in the middle leaves exactly the completed prefix recorded.
    try {
        uint32_t offset = 0;
        for (; offset + 4u <= sizeBytes; offset += 4u) {
            Memory::Write32(dl.writePtr, ReadBE32(data + offset));
            dl.writePtr += 4u;
            dl.count += 4u;
        }
        if (offset + 2u <= sizeBytes) {
            Memory::Write16(dl.writePtr, ReadBE16(data + offset));
            dl.writePtr += 2u;
            dl.count += 2u;
            offset += 2u;
        }
        if (offset < sizeBytes) {
            Memory::Write8(dl.writePtr, data[offset]);
            dl.writePtr += 1u;
            dl.count += 1u;
        }
    } catch (const Memory::AccessViolation&) {
    }
    return true;
}

extern "C" void GX_HLE_FIFO_WriteBurst(const uint8_t* data, uint32_t sizeBytes) {
    if (data == nullptr || sizeBytes == 0) {
        return;
    }

    if (IsDisplayListActive() && WriteDisplayListBurst(data, sizeBytes)) {
        return;
    }
    if (g_hleGxState.rawBegin && !IsDisplayListActive()) {
        const uint32_t taken = RawBeginAppend(data, sizeBytes);
        data += taken;
        sizeBytes -= taken;
        if (sizeBytes == 0) {
            return;
        }
    }

    const uint32_t applied = ApplyFifoPacketsDirect(data, sizeBytes);
    if (applied < sizeBytes) {
        HleFifoWriteBurstChunked(data + applied, sizeBytes - applied);
    }
}
