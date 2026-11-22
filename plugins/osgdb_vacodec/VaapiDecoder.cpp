#include <osg/Notify>
#include <cstring>
#include <algorithm>
#include <map>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <drm/drm_fourcc.h>
#include "VaapiDecoder.h"
using namespace osgVerse;

namespace
{
    /** Some drivers (the MTT one for instance) put VA fourcc values such as VA_FOURCC_ARGB in
        VADRMPRIMESurfaceDescriptor::fourcc, but EGL only accepts DRM fourcc values like
        DRM_FORMAT_ARGB8888 ('AR24'). Map the VA RGB formats to their DRM equivalents; values
        that already are DRM fourcc (NV12 etc.) are passed through unchanged. */
    uint32_t toDRMFourcc(uint32_t fourcc)
    {
        switch (fourcc)
        {
        case 0x42475241: return DRM_FORMAT_ARGB8888;  // VA_FOURCC_ARGB ('A','R','G','B')
        case 0x52474241: return DRM_FORMAT_ABGR8888;  // VA_FOURCC_ABGR
        case 0x41475242: return DRM_FORMAT_BGRA8888;  // VA_FOURCC_BGRA
        case 0x41424752: return DRM_FORMAT_RGBA8888;  // VA_FOURCC_RGBA
        case 0x42475258: return DRM_FORMAT_XRGB8888;  // VA_FOURCC_XRGB
        case 0x58424752: return DRM_FORMAT_RGBX8888;  // VA_FOURCC_RGBX
        case 0x58475242: return DRM_FORMAT_BGRX8888;  // VA_FOURCC_BGRX
        case 0x52474258: return DRM_FORMAT_XBGR8888;  // VA_FOURCC_XBGR
        default: return fourcc;
        }
    }

    /** Bit reader over an unescaped RBSP buffer. */
    struct BitReader
    {
        const uint8_t* data; int size; int bitPos; bool error;

        BitReader(const uint8_t* d, int s) : data(d), size(s), bitPos(0), error(false) {}

        int bitsLeft() const { return size * 8 - bitPos; }

        uint32_t u(int n)
        {
            uint32_t v = 0;
            for (int i = 0; i < n; ++i)
            {
                if (bitPos >= size * 8) { error = true; return 0; }
                v = (v << 1) | ((data[bitPos >> 3] >> (7 - (bitPos & 7))) & 1);
                ++bitPos;
            }
            return v;
        }

        uint32_t ue()
        {
            int zeros = 0;
            while (!error && bitsLeft() > 0 && u(1) == 0)
            { if (++zeros > 31) { error = true; return 0; } }
            if (error) return 0;
            return ((1u << zeros) - 1) + (zeros ? u(zeros) : 0);
        }

        int32_t se()
        {
            uint32_t v = ue();
            return (v & 1) ? (int32_t)((v + 1) / 2) : -(int32_t)(v / 2);
        }

        void skip(int n) { bitPos += n; if (bitPos > size * 8) error = true; }
    };

    /** Remove the emulation-prevention bytes (0x000003) from a NAL unit payload. */
    void unescapeRbsp(const uint8_t* src, int size, std::vector<uint8_t>& out)
    {
        out.clear(); out.reserve(size);
        int zeros = 0;
        for (int i = 0; i < size; ++i)
        {
            uint8_t b = src[i];
            if (zeros >= 2 && b == 0x03) { zeros = 0; continue; }
            out.push_back(b);
            zeros = (b == 0) ? (zeros + 1) : 0;
        }
    }

    /** Raw bit position (relative to the NAL start) corresponding to 'rbspBits' of the unescaped
        stream. Used to compute VASliceParameterBufferH264::slice_data_bit_offset, which is
        bit-exact and must skip the emulation-prevention bytes in the original NAL. */
    int rawBitOffsetForRbspBits(const uint8_t* raw, int rawSize, int rbspBits)
    {
        int rawPos = 0, produced = 0, zeros = 0;
        while (rawPos < rawSize && produced < rbspBits)
        {
            uint8_t b = raw[rawPos++];
            if (zeros >= 2 && b == 0x03) { zeros = 0; continue; }
            produced += 8; zeros = (b == 0) ? (zeros + 1) : 0;
        }
        return rawPos * 8 - (produced - rbspBits);
    }

    /** scaling_list() consumes the values; they are not used by this decoder (flat matrices). */
    void skipScalingList(BitReader& br, int size)
    {
        int lastScale = 8, nextScale = 8;
        for (int j = 0; j < size; ++j)
        {
            if (nextScale != 0)
            {
                int delta = br.se();
                nextScale = (lastScale + delta + 256) % 256;
            }
            if (nextScale != 0) lastScale = nextScale;
        }
    }

    /** True if the RBSP still contains syntax elements before its trailing stop bit. */
    bool moreRbspData(const uint8_t* data, int size, int bitPos)
    {
        int last = size * 8 - 1;
        while (last >= 0 && !((data[last >> 3] >> (7 - (last & 7))) & 1)) --last;
        return bitPos < last;
    }
}

namespace osgVerse
{
    /** Minimal host-side H.264 front-end for VA-API VLD: SPS/PPS/slice parsing, access unit
        assembly, reference picture management and submitting the VA parameter buffers. Current
        limitations: progressive frame pictures, 8-bit 4:2:0, I/P slices only (B slices are
        skipped), no scaling matrices (consumed but not sent). */
    class VaapiH264Decoder
    {
    public:
        explicit VaapiH264Decoder(VaapiDecoder* d)
        : _d(d), _hasVcl(false), _auPts(0), _frameIdx(0), _prevFrameNum(0),
          _prevFrameNumOffset(0), _prevPocLsb(0), _prevPocMsb(0), _maxLongTermFrameIdx(-1),
          _pendingFree(VA_INVALID_SURFACE), _decWidth(0), _decHeight(0),
          _warnedUnsupported(false), _warnedScaling(false), _warnedModification(false)
        {}

        ~VaapiH264Decoder() { releaseSurfaces(); }

        bool decode(const uint8_t* data, int size, long long pts);

        /** Destroy the decoded-picture surface pool (contexts are owned by VaapiDecoder). */
        void releaseSurfaces();

    private:
        struct SPS
        {
            int id = 0, profile = 0, level = 0, chromaFormat = 1;
            int bitDepthLumaMinus8 = 0, bitDepthChromaMinus8 = 0;
            int log2MaxFrameNumMinus4 = 0, pocType = 0, log2MaxPocLsbMinus4 = 0;
            bool deltaPocAlwaysZero = false;
            int offsetForNonRefPic = 0, offsetForTopToBottom = 0, numRefFramesInCycle = 0;
            std::vector<int> offsetForRefFrame;
            int maxNumRefFrames = 1;
            bool gapsAllowed = false;
            int picWidthInMbsMinus1 = 0, picHeightInMapUnitsMinus1 = 0;
            bool frameMbsOnly = true, mbAff = false, direct8x8 = true;
            int cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
        };

        struct PPS
        {
            int id = 0, spsId = 0;
            bool entropy = false, bottomFieldPocPresent = false;
            int numRefL0Default = 0, numRefL1Default = 0;
            bool weightedPred = false;
            int weightedBipred = 0;
            int picInitQpMinus26 = 0, picInitQsMinus26 = 0;
            int chromaQpOffset = 0, secondChromaQpOffset = 0;
            bool deblockingPresent = true, constrainedIntra = false, redundantPicCnt = false;
            bool transform8x8 = false;
        };

        struct Slice
        {
            int nalType = 0, nalRefIdc = 0;
            bool idr = false;
            int firstMb = 0, sliceTypeRaw = 0, sliceType = 0;  // sliceType is normalized to 0..4
            int ppsId = 0, spsId = 0;
            int frameNum = 0;
            int pocLsb = 0, deltaPoc0 = 0, deltaPoc1 = 0;
            int numRefL0 = 1, numRefL1 = 1;
            bool adaptiveMarking = false, longTermReferenceFlag = false, noOutputOfPriorPics = false;
            struct Mod { int idc, value; };
            std::vector<Mod> listModL0;
            struct Mmco { int op, arg1, arg2; };
            std::vector<Mmco> mmco;
            bool weighted = false;
            int lumaLog2WeightDenom = 0, chromaLog2WeightDenom = 0;
            bool lumaWeightFlagL0 = false, chromaWeightFlagL0 = false;
            int lumaWeightL0[32] = {0}, lumaOffsetL0[32] = {0};
            int chromaWeightL0[32][2] = {{0}}, chromaOffsetL0[32][2] = {{0}};
            int cabacInitIdc = 0;
            int sliceQpDelta = 0;
            int disableDeblocking = 0, alphaOffset = 0, betaOffset = 0;
            int bitOffset = 0;
        };

        struct Ref
        {
            VASurfaceID surface = VA_INVALID_SURFACE;
            int frameNum = 0, poc = 0, frameIdx = 0, longTermIdx = -1;
            bool longTerm = false;
        };

        bool addNal(const uint8_t* nal, int size, long long pts);
        bool submitAccessUnit(long long pts);
        void clearAccessUnit();

        bool parseSPS(const uint8_t* nal, int size);
        bool parsePPS(const uint8_t* nal, int size);
        bool parseSlicePrefix(const uint8_t* nal, int size, int& firstMb, int& sliceType, int& ppsId);
        bool parseSliceHeader(const uint8_t* nal, int size, Slice& sh);
        bool computePoc(const Slice& sh, const SPS& sps, int& frameNumOffset, int& pocMsb, int& poc);

        bool ensureResources(const SPS& sps);
        VASurfaceID allocSurface();
        void releaseSurface(VASurfaceID id);
        void updateDpb(const Slice& sh, const SPS& sps, VASurfaceID cur,
                       int frameNum, int frameNumOffset, int poc, int pocMsb);
        int buildRefList(const Slice& sh, const SPS& sps, VAPictureH264* list, int maxCount);
        void fillPicture(const Ref& ref, VAPictureH264& pic) const;
        void resetPocState();

        VaapiDecoder* _d;
        std::map<int, SPS> _sps;
        std::map<int, PPS> _pps;

        std::vector<std::vector<uint8_t>> _auNals;
        bool _hasVcl;
        long long _auPts;

        std::vector<Ref> _dpb;             // decoded reference pictures (frame pictures only)
        std::vector<VASurfaceID> _pool;    // YUV decode surfaces
        std::vector<char> _used;           // 1 if the surface is in use (current or reference)
        VASurfaceID _pendingFree;
        int _decWidth, _decHeight;

        int _frameIdx, _prevFrameNum, _prevFrameNumOffset;
        int _prevPocLsb, _prevPocMsb;
        int _maxLongTermFrameIdx;

        bool _warnedUnsupported, _warnedScaling, _warnedModification;
    };

    /////////////////////////////////////////////////////////////////////////
    // NAL / access unit handling

    bool VaapiH264Decoder::decode(const uint8_t* data, int size, long long pts)
    {
        if (!_d->_initialized || !data || size <= 0) return false;
        if (_d->_videoCodec != 4)  // osgVerse::CODEC_H264
        {
            if (!_warnedUnsupported)
            {
                _warnedUnsupported = true;
                OSG_WARN << "[VaapiDecoder] Only H.264 is implemented for VA-API hardware decoding"
                         << std::endl;
            }
            return false;
        }

        // Split the incoming buffer into NAL units (start-code based). Demuxers may deliver one
        // NAL per call (verse_video) or a whole access unit per call (ffmpeg).
        struct Span { int scStart, payloadStart; };
        std::vector<Span> spans;
        for (int i = 0; i + 3 <= size; )
        {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            { Span s = { i, i + 3 }; spans.push_back(s); i += 3; }
            else if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 &&
                     data[i + 2] == 0 && data[i + 3] == 1)
            { Span s = { i, i + 4 }; spans.push_back(s); i += 4; }
            else ++i;
        }
        if (spans.empty()) { Span s = { 0, 0 }; spans.push_back(s); }  // raw NAL without start code

        bool submitted = false;
        for (size_t k = 0; k < spans.size(); ++k)
        {
            int start = spans[k].payloadStart;
            int end = (k + 1 < spans.size()) ? spans[k + 1].scStart : size;
            if (end > start) submitted |= addNal(data + start, end - start, pts);
        }
        return submitted;
    }

    bool VaapiH264Decoder::addNal(const uint8_t* nal, int size, long long pts)
    {
        if (size <= 0) return false;
        int type = nal[0] & 0x1F;
        if (type == 7) { parseSPS(nal, size); return false; }
        if (type == 8) { parsePPS(nal, size); return false; }
        if (type == 9)  // access unit delimiter
        {
            if (_hasVcl) { bool ok = submitAccessUnit(_auPts); clearAccessUnit(); return ok; }
            return false;
        }
        if (type == 6 || type == 12) return false;  // SEI / filler

        if (type == 1 || type == 5)
        {
            int firstMb = 0, sliceType = 0, ppsId = 0;
            if (!parseSlicePrefix(nal, size, firstMb, sliceType, ppsId)) return false;

            bool submitted = false;
            if (firstMb == 0 && _hasVcl)
            { submitted = submitAccessUnit(_auPts); clearAccessUnit(); }

            if (_auNals.empty()) _auPts = pts;
            _auNals.emplace_back(nal, nal + size);
            _hasVcl = true;
            return submitted;
        }
        return false;
    }

    void VaapiH264Decoder::clearAccessUnit()
    {
        _auNals.clear(); _hasVcl = false;
    }

    /////////////////////////////////////////////////////////////////////////
    // SPS / PPS / slice header parsing

    bool VaapiH264Decoder::parseSPS(const uint8_t* nal, int size)
    {
        std::vector<uint8_t> rbsp; unescapeRbsp(nal, size, rbsp);
        if (rbsp.size() < 4) return false;

        BitReader br(rbsp.data(), (int)rbsp.size()); br.skip(8);
        SPS sps;
        sps.profile = br.u(8); br.u(8); sps.level = br.u(8); sps.id = br.ue();
        if (br.error) return false;

        bool highProfile = (sps.profile == 100 || sps.profile == 110 || sps.profile == 122 ||
                            sps.profile == 244 || sps.profile == 44 || sps.profile == 83 ||
                            sps.profile == 86 || sps.profile == 118 || sps.profile == 128 ||
                            sps.profile == 138 || sps.profile == 139 || sps.profile == 134 ||
                            sps.profile == 135);
        if (highProfile)
        {
            sps.chromaFormat = br.ue();
            if (sps.chromaFormat == 3) br.u(1);
            sps.bitDepthLumaMinus8 = br.ue();
            sps.bitDepthChromaMinus8 = br.ue();
            br.u(1);  // qpprime_y_zero_transform_bypass_flag
            if (br.u(1))  // seq_scaling_matrix_present_flag
            {
                int count = (sps.chromaFormat != 3) ? 8 : 12;
                for (int i = 0; i < count; ++i)
                    if (br.u(1)) skipScalingList(br, (i < 6) ? 16 : 64);
                if (!_warnedScaling)
                {
                    _warnedScaling = true;
                    OSG_WARN << "[VaapiDecoder] Custom scaling matrices are not supported, "
                             << "a flat VAIQMatrixBuffer is sent instead" << std::endl;
                }
            }
        }

        sps.log2MaxFrameNumMinus4 = br.ue();
        sps.pocType = br.ue();
        if (sps.pocType == 0)
            sps.log2MaxPocLsbMinus4 = br.ue();
        else if (sps.pocType == 1)
        {
            sps.deltaPocAlwaysZero = br.u(1);
            sps.offsetForNonRefPic = br.se();
            sps.offsetForTopToBottom = br.se();
            sps.numRefFramesInCycle = br.ue();
            sps.offsetForRefFrame.resize(sps.numRefFramesInCycle);
            for (int i = 0; i < sps.numRefFramesInCycle; ++i)
                sps.offsetForRefFrame[i] = br.se();
        }

        sps.maxNumRefFrames = br.ue();
        sps.gapsAllowed = br.u(1);
        sps.picWidthInMbsMinus1 = br.ue();
        sps.picHeightInMapUnitsMinus1 = br.ue();
        sps.frameMbsOnly = br.u(1);
        if (!sps.frameMbsOnly) sps.mbAff = br.u(1);
        sps.direct8x8 = br.u(1);
        if (br.u(1))  // frame_cropping_flag
        {
            sps.cropLeft = br.ue(); sps.cropRight = br.ue();
            sps.cropTop = br.ue(); sps.cropBottom = br.ue();
        }
        if (br.error) return false;

        if (sps.chromaFormat != 1 || sps.bitDepthLumaMinus8 != 0 || sps.bitDepthChromaMinus8 != 0)
        {
            if (!_warnedUnsupported)
            {
                _warnedUnsupported = true;
                OSG_WARN << "[VaapiDecoder] Only 8-bit 4:2:0 H.264 is supported (chroma_format="
                         << sps.chromaFormat << ")" << std::endl;
            }
            return false;
        }
        _sps[sps.id] = sps; return true;
    }

    bool VaapiH264Decoder::parsePPS(const uint8_t* nal, int size)
    {
        std::vector<uint8_t> rbsp; unescapeRbsp(nal, size, rbsp);
        if (rbsp.size() < 2) return false;

        BitReader br(rbsp.data(), (int)rbsp.size()); br.skip(8);
        PPS pps;
        pps.id = br.ue(); pps.spsId = br.ue();
        pps.entropy = br.u(1);
        pps.bottomFieldPocPresent = br.u(1);
        int sliceGroups = br.ue();
        if (sliceGroups > 0)
        {
            if (!_warnedUnsupported)
            {
                _warnedUnsupported = true;
                OSG_WARN << "[VaapiDecoder] H.264 slice groups (FMO) are not supported" << std::endl;
            }
            return false;
        }
        pps.numRefL0Default = br.ue();
        pps.numRefL1Default = br.ue();
        pps.weightedPred = br.u(1);
        pps.weightedBipred = br.u(2);
        pps.picInitQpMinus26 = br.se();
        pps.picInitQsMinus26 = br.se();
        pps.chromaQpOffset = br.se();
        pps.deblockingPresent = br.u(1);
        pps.constrainedIntra = br.u(1);
        pps.redundantPicCnt = br.u(1);
        pps.secondChromaQpOffset = pps.chromaQpOffset;

        if (!br.error && moreRbspData(rbsp.data(), (int)rbsp.size(), br.bitPos))
        {
            pps.transform8x8 = br.u(1);
            if (br.u(1))  // pic_scaling_matrix_present_flag
            {
                int chroma = 1;
                std::map<int, SPS>::const_iterator it = _sps.find(pps.spsId);
                if (it != _sps.end()) chroma = it->second.chromaFormat;
                int count = (chroma != 3) ? 8 : 12;
                for (int i = 0; i < count; ++i)
                    if (br.u(1)) skipScalingList(br, (i < 6) ? 16 : 64);
                if (!_warnedScaling)
                {
                    _warnedScaling = true;
                    OSG_WARN << "[VaapiDecoder] Custom scaling matrices are not supported, "
                             << "a flat VAIQMatrixBuffer is sent instead" << std::endl;
                }
            }
            pps.secondChromaQpOffset = br.se();
        }
        if (br.error) return false;
        _pps[pps.id] = pps; return true;
    }

    bool VaapiH264Decoder::parseSlicePrefix(const uint8_t* nal, int size, int& firstMb,
                                            int& sliceType, int& ppsId)
    {
        std::vector<uint8_t> rbsp; unescapeRbsp(nal, size, rbsp);
        if (rbsp.size() < 2) return false;

        BitReader br(rbsp.data(), (int)rbsp.size()); br.skip(8);
        firstMb = br.ue(); sliceType = br.ue(); ppsId = br.ue();
        return !br.error;
    }

    bool VaapiH264Decoder::parseSliceHeader(const uint8_t* nal, int size, Slice& sh)
    {
        std::vector<uint8_t> rbsp; unescapeRbsp(nal, size, rbsp);
        if (rbsp.size() < 2) return false;

        sh.nalType = nal[0] & 0x1F;
        sh.nalRefIdc = (nal[0] >> 5) & 3;
        sh.idr = (sh.nalType == 5);

        BitReader br(rbsp.data(), (int)rbsp.size()); br.skip(8);
        sh.firstMb = br.ue();
        sh.sliceTypeRaw = br.ue();
        sh.sliceType = sh.sliceTypeRaw % 5;
        sh.ppsId = br.ue();
        if (br.error) return false;

        std::map<int, PPS>::const_iterator ppsIt = _pps.find(sh.ppsId);
        if (ppsIt == _pps.end()) return false;
        const PPS& pps = ppsIt->second;
        std::map<int, SPS>::const_iterator spsIt = _sps.find(pps.spsId);
        if (spsIt == _sps.end()) return false;
        const SPS& sps = spsIt->second;
        sh.spsId = pps.spsId;

        if (sh.sliceType == 1 || sh.sliceType == 3 || sh.sliceType == 4)
        {
            if (!_warnedUnsupported)
            {
                _warnedUnsupported = true;
                OSG_WARN << "[VaapiDecoder] H.264 B/SP/SI slices are not supported yet" << std::endl;
            }
            return false;
        }

        sh.frameNum = br.u(sps.log2MaxFrameNumMinus4 + 4);
        if (!sps.frameMbsOnly)
        {
            if (br.u(1))  // field_pic_flag
            {
                if (!_warnedUnsupported)
                {
                    _warnedUnsupported = true;
                    OSG_WARN << "[VaapiDecoder] H.264 field pictures are not supported yet"
                             << std::endl;
                }
                return false;
            }
        }
        if (sh.idr) br.ue();  // idr_pic_id

        if (sps.pocType == 0)
        {
            sh.pocLsb = br.u(sps.log2MaxPocLsbMinus4 + 4);
            if (pps.bottomFieldPocPresent && sps.frameMbsOnly == true)
                br.se();  // delta_pic_order_cnt_bottom (frame pictures: ignored)
        }
        else if (sps.pocType == 1 && !sps.deltaPocAlwaysZero)
        {
            sh.deltaPoc0 = br.se();
            if (pps.bottomFieldPocPresent) sh.deltaPoc1 = br.se();
        }
        if (pps.redundantPicCnt) br.ue();  // redundant_pic_cnt

        sh.numRefL0 = pps.numRefL0Default + 1;
        sh.numRefL1 = pps.numRefL1Default + 1;
        if (sh.sliceType == 0)  // P slice
        {
            if (br.u(1))  // num_ref_idx_active_override_flag
            {
                sh.numRefL0 = br.ue() + 1;
                if (sh.sliceType == 1) sh.numRefL1 = br.ue() + 1;
            }

            if (br.u(1))  // ref_pic_list_modification_flag_l0
            {
                while (!br.error)
                {
                    int idc = br.ue();
                    if (idc == 3) break;
                    Slice::Mod m; m.idc = idc; m.value = 0;
                    if (idc == 0 || idc == 1 || idc == 2) m.value = br.ue();
                    else { br.error = true; break; }
                    sh.listModL0.push_back(m);
                }
                if (!sh.listModL0.empty()) _warnedModification = true;
            }
        }

        if (pps.weightedPred && sh.sliceType == 0)
        {
            sh.weighted = true;
            sh.lumaLog2WeightDenom = br.ue();
            if (sps.chromaFormat != 0) sh.chromaLog2WeightDenom = br.ue();

            sh.lumaWeightFlagL0 = br.u(1);
            if (sh.lumaWeightFlagL0)
            {
                for (int i = 0; i < sh.numRefL0 && i < 32; ++i)
                {
                    sh.lumaWeightL0[i] = br.se();
                    sh.lumaOffsetL0[i] = br.se();
                }
            }
            sh.chromaWeightFlagL0 = br.u(1);
            if (sh.chromaWeightFlagL0)
            {
                for (int i = 0; i < sh.numRefL0 && i < 32; ++i)
                {
                    for (int j = 0; j < 2; ++j)
                    {
                        sh.chromaWeightL0[i][j] = br.se();
                        sh.chromaOffsetL0[i][j] = br.se();
                    }
                }
            }
        }

        if (sh.nalRefIdc != 0)
        {
            if (sh.idr)
            {
                sh.noOutputOfPriorPics = br.u(1);
                sh.longTermReferenceFlag = br.u(1);
            }
            else
            {
                sh.adaptiveMarking = br.u(1);
                if (sh.adaptiveMarking)
                {
                    while (!br.error)
                    {
                        Slice::Mmco m; m.op = br.ue(); m.arg1 = m.arg2 = 0;
                        if (m.op == 0) break;
                        if (m.op == 2 || m.op == 3 || m.op == 6) m.arg1 = br.ue();
                        if (m.op == 1 || m.op == 3) m.arg2 = br.ue();
                        if (m.op < 1 || m.op > 6) { br.error = true; break; }
                        sh.mmco.push_back(m);
                    }
                }
            }
        }

        if (pps.entropy && sh.sliceType != 2 && sh.sliceType != 4)
            sh.cabacInitIdc = br.ue();
        sh.sliceQpDelta = br.se();
        if (pps.deblockingPresent)
        {
            sh.disableDeblocking = br.ue();
            if (sh.disableDeblocking != 1)
            {
                sh.alphaOffset = br.se();
                sh.betaOffset = br.se();
            }
        }
        if (br.error) return false;

        // slice_data_bit_offset is relative to the NAL unit and includes its header byte
        sh.bitOffset = rawBitOffsetForRbspBits(nal, size, br.bitPos);
        return true;
    }

    /////////////////////////////////////////////////////////////////////////
    // POC / DPB

    bool VaapiH264Decoder::computePoc(const Slice& sh, const SPS& sps,
                                      int& frameNumOffset, int& pocMsb, int& poc)
    {
        int maxFrameNum = 1 << (sps.log2MaxFrameNumMinus4 + 4);
        if (sh.idr) frameNumOffset = 0;
        else frameNumOffset = (_prevFrameNum > sh.frameNum) ?
                              (_prevFrameNumOffset + maxFrameNum) : _prevFrameNumOffset;

        if (sps.pocType == 0)
        {
            int maxPocLsb = 1 << (sps.log2MaxPocLsbMinus4 + 4);
            if (sh.idr) { pocMsb = 0; poc = sh.pocLsb; }
            else
            {
                if (sh.pocLsb < _prevPocLsb && (_prevPocLsb - sh.pocLsb) >= maxPocLsb / 2)
                    pocMsb = _prevPocMsb + maxPocLsb;
                else if (sh.pocLsb > _prevPocLsb && (sh.pocLsb - _prevPocLsb) > maxPocLsb / 2)
                    pocMsb = _prevPocMsb - maxPocLsb;
                else
                    pocMsb = _prevPocMsb;
                poc = pocMsb + sh.pocLsb;
            }
        }
        else if (sps.pocType == 1)
        {
            int absFrameNum = frameNumOffset + sh.frameNum;
            if (sps.maxNumRefFrames == 0) absFrameNum = 0;
            int expectedPoc = 0;
            if (absFrameNum > 0 && sps.numRefFramesInCycle > 0)
            {
                int cycleCnt = (absFrameNum - 1) / sps.numRefFramesInCycle;
                int inCycle = (absFrameNum - 1) % sps.numRefFramesInCycle;
                int sum = 0;
                for (int i = 0; i < sps.numRefFramesInCycle; ++i) sum += sps.offsetForRefFrame[i];
                expectedPoc = cycleCnt * sum;
                for (int i = 0; i <= inCycle; ++i) expectedPoc += sps.offsetForRefFrame[i];
            }
            if (sh.nalRefIdc == 0) expectedPoc += sps.offsetForNonRefPic;
            poc = expectedPoc + sh.deltaPoc0;
            pocMsb = 0;
        }
        else
        {
            poc = 2 * (frameNumOffset + sh.frameNum) - ((sh.nalRefIdc == 0) ? 1 : 0);
            pocMsb = 0;
        }
        return true;
    }

    void VaapiH264Decoder::resetPocState()
    {
        _prevFrameNum = 0; _prevFrameNumOffset = 0;
        _prevPocLsb = 0; _prevPocMsb = 0;
    }

    void VaapiH264Decoder::fillPicture(const Ref& ref, VAPictureH264& pic) const
    {
        memset(&pic, 0, sizeof(pic));
        pic.picture_id = ref.surface;
        pic.frame_idx = (uint32_t)ref.frameIdx;
        pic.flags = ref.longTerm ? VA_PICTURE_H264_LONG_TERM_REFERENCE
                                 : VA_PICTURE_H264_SHORT_TERM_REFERENCE;
        pic.TopFieldOrderCnt = ref.poc;
        pic.BottomFieldOrderCnt = ref.poc;
    }

    int VaapiH264Decoder::buildRefList(const Slice& sh, const SPS& sps,
                                       VAPictureH264* list, int maxCount)
    {
        for (int i = 0; i < maxCount; ++i)
        {
            memset(&list[i], 0, sizeof(VAPictureH264));
            list[i].picture_id = VA_INVALID_SURFACE;
            list[i].flags = VA_PICTURE_H264_INVALID;
        }
        if (sh.sliceType == 2 || sh.sliceType == 4) return 0;  // I / SI

        // Implicit list: short-term refs by descending POC, then long-term refs
        std::vector<const Ref*> ordered;
        std::vector<const Ref*> shorts, longs;
        for (size_t i = 0; i < _dpb.size(); ++i)
        {
            if (_dpb[i].longTerm) longs.push_back(&_dpb[i]);
            else shorts.push_back(&_dpb[i]);
        }
        std::sort(shorts.begin(), shorts.end(),
                  [](const Ref* a, const Ref* b) { return a->poc > b->poc; });
        std::sort(longs.begin(), longs.end(),
                  [](const Ref* a, const Ref* b) { return a->longTermIdx < b->longTermIdx; });
        ordered.insert(ordered.end(), shorts.begin(), shorts.end());
        ordered.insert(ordered.end(), longs.begin(), longs.end());

        int need = std::max(1, sh.numRefL0);
        if (ordered.empty())
        {
            if (!_warnedUnsupported)
            {
                _warnedUnsupported = true;
                OSG_WARN << "[VaapiDecoder] P slice without reference pictures" << std::endl;
            }
            return 0;
        }
        while ((int)ordered.size() < need) ordered.push_back(ordered.back());

        // Apply ref_pic_list_modification (frame pictures)
        int maxFrameNum = 1 << (sps.log2MaxFrameNumMinus4 + 4);
        int curIdx = 0;
        for (size_t m = 0; m < sh.listModL0.size() && curIdx < need; ++m)
        {
            const Slice::Mod& mod = sh.listModL0[m];
            int found = -1;
            if (mod.idc == 0 || mod.idc == 1)
            {
                int picNumX = sh.frameNum - (mod.value + 1);
                for (size_t j = 0; j < ordered.size(); ++j)
                {
                    const Ref* r = ordered[j];
                    int picNum = (r->frameNum > sh.frameNum) ? r->frameNum - maxFrameNum : r->frameNum;
                    bool match = (mod.idc == 0) ? (!r->longTerm && picNum == picNumX)
                                                : (r->longTerm && picNum == picNumX);
                    if (match) { found = (int)j; break; }
                }
            }
            else if (mod.idc == 2)
            {
                for (size_t j = 0; j < ordered.size(); ++j)
                    if (ordered[j]->longTerm && ordered[j]->longTermIdx == mod.value)
                    { found = (int)j; break; }
            }
            if (found >= 0 && found != curIdx)
            {
                const Ref* r = ordered[found];
                ordered.erase(ordered.begin() + found);
                ordered.insert(ordered.begin() + curIdx, r);
            }
            ++curIdx;
        }

        int count = std::min(need, maxCount);
        for (int i = 0; i < count; ++i) fillPicture(*ordered[i], list[i]);
        return count;
    }

    void VaapiH264Decoder::updateDpb(const Slice& sh, const SPS& sps, VASurfaceID cur,
                                     int frameNum, int frameNumOffset, int poc, int pocMsb)
    {
        bool ref = (sh.nalRefIdc != 0);
        bool curLongTerm = false;
        int curLongTermIdx = -1;

        if (sh.idr)
        {
            for (size_t i = 0; i < _dpb.size(); ++i) releaseSurface(_dpb[i].surface);
            _dpb.clear();
            _maxLongTermFrameIdx = -1;
            if (sh.longTermReferenceFlag)
            { curLongTerm = true; curLongTermIdx = 0; _maxLongTermFrameIdx = 0; }
            resetPocState();
        }
        else if (ref)
        {
            int maxFrameNum = 1 << (sps.log2MaxFrameNumMinus4 + 4);
            if (sh.adaptiveMarking)
            {
                for (size_t m = 0; m < sh.mmco.size(); ++m)
                {
                    const Slice::Mmco& op = sh.mmco[m];
                    if (op.op == 1 || op.op == 3)
                    {
                        int picNumX = frameNum - (op.arg2 + 1);
                        for (size_t i = 0; i < _dpb.size(); ++i)
                        {
                            int picNum = (_dpb[i].frameNum > frameNum) ?
                                         _dpb[i].frameNum - maxFrameNum : _dpb[i].frameNum;
                            if (!_dpb[i].longTerm && picNum == picNumX)
                            {
                                if (op.op == 1) { releaseSurface(_dpb[i].surface); _dpb.erase(_dpb.begin() + i); }
                                else { _dpb[i].longTerm = true; _dpb[i].longTermIdx = op.arg1; }
                                break;
                            }
                        }
                    }
                    else if (op.op == 2)
                    {
                        for (size_t i = 0; i < _dpb.size(); ++i)
                            if (_dpb[i].longTerm && _dpb[i].longTermIdx == op.arg1)
                            { releaseSurface(_dpb[i].surface); _dpb.erase(_dpb.begin() + i); break; }
                    }
                    else if (op.op == 4) _maxLongTermFrameIdx = op.arg1 - 1;
                    else if (op.op == 5)
                    {
                        for (size_t i = 0; i < _dpb.size(); ++i) releaseSurface(_dpb[i].surface);
                        _dpb.clear(); _maxLongTermFrameIdx = -1;
                    }
                    else if (op.op == 6)
                    {
                        curLongTerm = true; curLongTermIdx = op.arg1;
                        _maxLongTermFrameIdx = std::max(_maxLongTermFrameIdx, curLongTermIdx);
                    }
                }
            }
            else if (sps.maxNumRefFrames > 0)
            {
                while ((int)_dpb.size() >= sps.maxNumRefFrames)
                {
                    size_t oldest = 0;
                    for (size_t i = 1; i < _dpb.size(); ++i)
                        if (_dpb[i].poc < _dpb[oldest].poc) oldest = i;
                    releaseSurface(_dpb[oldest].surface);
                    _dpb.erase(_dpb.begin() + oldest);
                }
            }
        }

        if (ref)
        {
            Ref r; r.surface = cur; r.frameNum = frameNum; r.poc = poc;
            r.frameIdx = curLongTerm ? curLongTermIdx : frameNum;
            r.longTerm = curLongTerm; r.longTermIdx = curLongTermIdx;
            _dpb.push_back(r);
        }
        else
            _pendingFree = cur;  // released at the beginning of the next access unit

        _prevFrameNum = frameNum;
        _prevFrameNumOffset = frameNumOffset;
        if (sps.pocType == 0) { _prevPocLsb = sh.pocLsb; _prevPocMsb = pocMsb; }
        ++_frameIdx;
    }

    /////////////////////////////////////////////////////////////////////////
    // VA surfaces / submission

    void VaapiH264Decoder::releaseSurface(VASurfaceID id)
    {
        if (id == VA_INVALID_SURFACE) return;
        for (size_t i = 0; i < _pool.size(); ++i)
            if (_pool[i] == id) { _used[i] = 0; return; }
    }

    VASurfaceID VaapiH264Decoder::allocSurface()
    {
        for (size_t i = 0; i < _pool.size(); ++i)
            if (!_used[i]) { _used[i] = 1; return _pool[i]; }
        return VA_INVALID_SURFACE;
    }

    void VaapiH264Decoder::releaseSurfaces()
    {
        if (_d && _d->_vaDisplay)
        {
            for (size_t i = 0; i < _pool.size(); ++i)
                vaDestroySurfaces(_d->_vaDisplay, &_pool[i], 1);
        }
        _pool.clear(); _used.clear(); _dpb.clear();
        _pendingFree = VA_INVALID_SURFACE;
        _decWidth = _decHeight = 0;
    }

    bool VaapiH264Decoder::ensureResources(const SPS& sps)
    {
        VaapiDecoder* d = _d;
        if (!d->_vaDisplay) return false;

        int w = (sps.picWidthInMbsMinus1 + 1) * 16;
        int h = (sps.picHeightInMapUnitsMinus1 + 1) * 16 * (sps.frameMbsOnly ? 1 : 2);
        int refs = std::max(0, std::min(sps.maxNumRefFrames, 16));
        if (!_pool.empty() && d->_vaContext != VA_INVALID_ID &&
            w == _decWidth && h == _decHeight && (int)_pool.size() >= refs + 2)
            return true;

        if (d->_vppContext != VA_INVALID_ID)
        { vaDestroyContext(d->_vaDisplay, d->_vppContext); d->_vppContext = VA_INVALID_ID; }
        if (d->_vaContext != VA_INVALID_ID)
        { vaDestroyContext(d->_vaDisplay, d->_vaContext); d->_vaContext = VA_INVALID_ID; }
        releaseSurfaces();
        if (d->_vaSurfaceRGB != VA_INVALID_SURFACE)
        {
            vaDestroySurfaces(d->_vaDisplay, &d->_vaSurfaceRGB, 1);
            d->_vaSurfaceRGB = VA_INVALID_SURFACE;
        }

        VAStatus status = vaCreateContext(d->_vaDisplay, d->_vaConfig, w, h,
                                          VA_PROGRESSIVE, NULL, 0, &d->_vaContext);
        if (status != VA_STATUS_SUCCESS)
        {
            OSG_WARN << "[VaapiDecoder] Failed to create decode context: "
                     << vaErrorStr(status) << std::endl;
            return false;
        }

        int count = std::min(16, std::max(2, refs + 2));
        std::vector<VASurfaceID> ids(count, VA_INVALID_SURFACE);
        VASurfaceAttrib attrib;
        memset(&attrib, 0, sizeof(attrib));
        attrib.type = VASurfaceAttribMemoryType;
        attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
        attrib.value.type = VAGenericValueTypeInteger;
        attrib.value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
        status = vaCreateSurfaces(d->_vaDisplay, VA_RT_FORMAT_YUV420, w, h,
                                  ids.data(), count, &attrib, 1);
        if (status != VA_STATUS_SUCCESS)
        {
            OSG_WARN << "[VaapiDecoder] Failed to create decode surfaces: "
                     << vaErrorStr(status) << std::endl;
            return false;
        }
        _pool = ids; _used.assign(count, 0);

        if (d->_useVPP && d->_vppConfig != VA_INVALID_ID)
        {
            VASurfaceAttrib rgbAttrib;
            memset(&rgbAttrib, 0, sizeof(rgbAttrib));
            rgbAttrib.type = VASurfaceAttribMemoryType;
            rgbAttrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
            rgbAttrib.value.type = VAGenericValueTypeInteger;
            rgbAttrib.value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
            status = vaCreateSurfaces(d->_vaDisplay, VA_RT_FORMAT_RGB32, w, h,
                                      &d->_vaSurfaceRGB, 1, &rgbAttrib, 1);
            if (status == VA_STATUS_SUCCESS)
            {
                status = vaCreateContext(d->_vaDisplay, d->_vppConfig, w, h, VA_PROGRESSIVE,
                                         &d->_vaSurfaceRGB, 1, &d->_vppContext);
            }
            if (status != VA_STATUS_SUCCESS)
            {
                OSG_WARN << "[VaapiDecoder] Failed to create VPP resources: "
                         << vaErrorStr(status) << ", RGB conversion will be disabled" << std::endl;
                if (d->_vaSurfaceRGB != VA_INVALID_SURFACE)
                {
                    vaDestroySurfaces(d->_vaDisplay, &d->_vaSurfaceRGB, 1);
                    d->_vaSurfaceRGB = VA_INVALID_SURFACE;
                }
                if (d->_vppContext != VA_INVALID_ID)
                { vaDestroyContext(d->_vaDisplay, d->_vppContext); d->_vppContext = VA_INVALID_ID; }
                d->_useVPP = false;
            }
        }

        _decWidth = w; _decHeight = h;
        d->_width = w; d->_height = h;
        if (sps.cropLeft || sps.cropRight || sps.cropTop || sps.cropBottom)
            OSG_WARN << "[VaapiDecoder] Picture cropping is ignored, using coded size "
                     << w << "x" << h << std::endl;
        return true;
    }

    bool VaapiH264Decoder::submitAccessUnit(long long pts)
    {
        // Free the previous non-reference surface (it is no longer needed after convert())
        if (_pendingFree != VA_INVALID_SURFACE)
        { releaseSurface(_pendingFree); _pendingFree = VA_INVALID_SURFACE; }

        std::vector<Slice> slices;
        for (size_t i = 0; i < _auNals.size(); ++i)
        {
            const std::vector<uint8_t>& nal = _auNals[i];
            int type = nal.empty() ? 0 : (nal[0] & 0x1F);
            if (type != 1 && type != 5) continue;
            Slice sh;
            if (!parseSliceHeader(nal.data(), (int)nal.size(), sh)) return false;
            slices.push_back(sh);
        }
        if (slices.empty()) return false;
        const Slice& first = slices[0];

        std::map<int, PPS>::const_iterator ppsIt = _pps.find(first.ppsId);
        std::map<int, SPS>::const_iterator spsIt =
            (ppsIt != _pps.end()) ? _sps.find(ppsIt->second.spsId) : _sps.end();
        if (ppsIt == _pps.end() || spsIt == _sps.end()) return false;
        const PPS& pps = ppsIt->second;
        const SPS& sps = spsIt->second;

        int frameNumOffset = 0, pocMsb = 0, poc = 0;
        if (!computePoc(first, sps, frameNumOffset, pocMsb, poc)) return false;
        if (!ensureResources(sps)) return false;

        VASurfaceID cur = allocSurface();
        if (cur == VA_INVALID_SURFACE)
        {
            OSG_WARN << "[VaapiDecoder] No free decode surface (max_num_ref_frames = "
                     << sps.maxNumRefFrames << ")" << std::endl;
            return false;
        }

        VAPictureParameterBufferH264 pic;
        memset(&pic, 0, sizeof(pic));
        pic.CurrPic.picture_id = cur;
        pic.CurrPic.frame_idx = (first.idr && first.longTermReferenceFlag) ?
                                0u : (uint32_t)first.frameNum;
        pic.CurrPic.flags = (first.nalRefIdc != 0) ?
                            ((first.idr && first.longTermReferenceFlag) ?
                             VA_PICTURE_H264_LONG_TERM_REFERENCE :
                             VA_PICTURE_H264_SHORT_TERM_REFERENCE) : 0;
        pic.CurrPic.TopFieldOrderCnt = poc;
        pic.CurrPic.BottomFieldOrderCnt = poc;

        for (int i = 0; i < 16; ++i)
        {
            pic.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
            pic.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
        }
        for (size_t i = 0; i < _dpb.size() && i < 16; ++i)
            fillPicture(_dpb[i], pic.ReferenceFrames[i]);

        pic.picture_width_in_mbs_minus1 = (uint16_t)sps.picWidthInMbsMinus1;
        pic.picture_height_in_mbs_minus1 =
            (uint16_t)((sps.picHeightInMapUnitsMinus1 + 1) * (sps.frameMbsOnly ? 1 : 2) - 1);
        pic.bit_depth_luma_minus8 = (uint8_t)sps.bitDepthLumaMinus8;
        pic.bit_depth_chroma_minus8 = (uint8_t)sps.bitDepthChromaMinus8;
        pic.num_ref_frames = (uint8_t)sps.maxNumRefFrames;
        pic.seq_fields.bits.chroma_format_idc = sps.chromaFormat;
        pic.seq_fields.bits.residual_colour_transform_flag = 0;
        pic.seq_fields.bits.gaps_in_frame_num_value_allowed_flag = sps.gapsAllowed ? 1 : 0;
        pic.seq_fields.bits.frame_mbs_only_flag = sps.frameMbsOnly ? 1 : 0;
        pic.seq_fields.bits.mb_adaptive_frame_field_flag = sps.mbAff ? 1 : 0;
        pic.seq_fields.bits.direct_8x8_inference_flag = sps.direct8x8 ? 1 : 0;
        pic.seq_fields.bits.MinLumaBiPredSize8x8 = (sps.level >= 31) ? 1 : 0;
        pic.seq_fields.bits.log2_max_frame_num_minus4 = sps.log2MaxFrameNumMinus4;
        pic.seq_fields.bits.pic_order_cnt_type = sps.pocType;
        pic.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = sps.log2MaxPocLsbMinus4;
        pic.seq_fields.bits.delta_pic_order_always_zero_flag = sps.deltaPocAlwaysZero ? 1 : 0;
        pic.pic_init_qp_minus26 = (int8_t)pps.picInitQpMinus26;
        pic.pic_init_qs_minus26 = (int8_t)pps.picInitQsMinus26;
        pic.chroma_qp_index_offset = (int8_t)pps.chromaQpOffset;
        pic.second_chroma_qp_index_offset = (int8_t)pps.secondChromaQpOffset;
        pic.pic_fields.bits.entropy_coding_mode_flag = pps.entropy ? 1 : 0;
        pic.pic_fields.bits.weighted_pred_flag = pps.weightedPred ? 1 : 0;
        pic.pic_fields.bits.weighted_bipred_idc = pps.weightedBipred;
        pic.pic_fields.bits.transform_8x8_mode_flag = pps.transform8x8 ? 1 : 0;
        pic.pic_fields.bits.field_pic_flag = 0;
        pic.pic_fields.bits.constrained_intra_pred_flag = pps.constrainedIntra ? 1 : 0;
        pic.pic_fields.bits.pic_order_present_flag = pps.bottomFieldPocPresent ? 1 : 0;
        pic.pic_fields.bits.deblocking_filter_control_present_flag = pps.deblockingPresent ? 1 : 0;
        pic.pic_fields.bits.redundant_pic_cnt_present_flag = pps.redundantPicCnt ? 1 : 0;
        pic.pic_fields.bits.reference_pic_flag = (first.nalRefIdc != 0) ? 1 : 0;
        pic.frame_num = (uint16_t)first.frameNum;

        std::vector<VABufferID> buffers;
        VABufferID buf = VA_INVALID_ID;
        VAStatus status = vaCreateBuffer(_d->_vaDisplay, _d->_vaContext,
                                         VAPictureParameterBufferType, sizeof(pic), 1, &pic, &buf);
        if (status != VA_STATUS_SUCCESS)
        {
            OSG_WARN << "[VaapiDecoder] Failed to create picture parameter buffer: "
                     << vaErrorStr(status) << std::endl;
            releaseSurface(cur); return false;
        }
        buffers.push_back(buf);

        // The IQ matrix has to be sent even when the stream carries no scaling lists (flat
        // value 16): some drivers (the MTT one) produce garbage or nothing without it
        VAIQMatrixBufferH264 iq;
        memset(&iq, 0, sizeof(iq));
        memset(iq.ScalingList4x4, 16, sizeof(iq.ScalingList4x4));
        memset(iq.ScalingList8x8, 16, sizeof(iq.ScalingList8x8));
        VABufferID iqBuf = VA_INVALID_ID;
        status = vaCreateBuffer(_d->_vaDisplay, _d->_vaContext, VAIQMatrixBufferType,
                                sizeof(iq), 1, &iq, &iqBuf);
        if (status == VA_STATUS_SUCCESS) buffers.push_back(iqBuf);

        for (size_t i = 0; i < slices.size(); ++i)
        {
            const Slice& sh = slices[i];
            const std::vector<uint8_t>& nal = _auNals[i];  // same order as the VCL scan above

            VASliceParameterBufferH264 slice;
            memset(&slice, 0, sizeof(slice));
            for (int j = 0; j < 32; ++j)
            {
                slice.RefPicList0[j].picture_id = VA_INVALID_SURFACE;
                slice.RefPicList0[j].flags = VA_PICTURE_H264_INVALID;
                slice.RefPicList1[j].picture_id = VA_INVALID_SURFACE;
                slice.RefPicList1[j].flags = VA_PICTURE_H264_INVALID;
            }
            slice.slice_data_size = (uint32_t)nal.size();
            slice.slice_data_offset = 0;
            slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
            slice.slice_data_bit_offset = (uint16_t)sh.bitOffset;
            slice.first_mb_in_slice = (uint16_t)sh.firstMb;
            slice.slice_type = (uint8_t)sh.sliceType;  // normalized 0..4 (VAAPI convention)
            slice.num_ref_idx_l0_active_minus1 =
                (uint8_t)std::max(0, buildRefList(sh, sps, slice.RefPicList0, 32) - 1);
            // (buildRefList also fills List0; L1 stays empty because B slices are not decoded)
            slice.cabac_init_idc = (uint8_t)sh.cabacInitIdc;
            slice.slice_qp_delta = (int8_t)sh.sliceQpDelta;
            slice.disable_deblocking_filter_idc = (uint8_t)sh.disableDeblocking;
            slice.slice_alpha_c0_offset_div2 = (int8_t)sh.alphaOffset;
            slice.slice_beta_offset_div2 = (int8_t)sh.betaOffset;
            if (sh.weighted)
            {
                slice.luma_log2_weight_denom = (uint8_t)sh.lumaLog2WeightDenom;
                slice.chroma_log2_weight_denom = (uint8_t)sh.chromaLog2WeightDenom;
                slice.luma_weight_l0_flag = sh.lumaWeightFlagL0 ? 1 : 0;
                slice.chroma_weight_l0_flag = sh.chromaWeightFlagL0 ? 1 : 0;
                for (int j = 0; j < 32; ++j)
                {
                    slice.luma_weight_l0[j] = (int16_t)sh.lumaWeightL0[j];
                    slice.luma_offset_l0[j] = (int16_t)sh.lumaOffsetL0[j];
                    slice.chroma_weight_l0[j][0] = (int16_t)sh.chromaWeightL0[j][0];
                    slice.chroma_weight_l0[j][1] = (int16_t)sh.chromaWeightL0[j][1];
                    slice.chroma_offset_l0[j][0] = (int16_t)sh.chromaOffsetL0[j][0];
                    slice.chroma_offset_l0[j][1] = (int16_t)sh.chromaOffsetL0[j][1];
                }
            }

            VABufferID paramBuf = VA_INVALID_ID;
            status = vaCreateBuffer(_d->_vaDisplay, _d->_vaContext,
                                    VASliceParameterBufferType, sizeof(slice), 1, &slice, &paramBuf);
            if (status != VA_STATUS_SUCCESS)
            {
                OSG_WARN << "[VaapiDecoder] Failed to create slice parameter buffer: "
                         << vaErrorStr(status) << std::endl;
                break;
            }
            buffers.push_back(paramBuf);

            VABufferID dataBuf = VA_INVALID_ID;
            status = vaCreateBuffer(_d->_vaDisplay, _d->_vaContext, VASliceDataBufferType,
                                    (unsigned int)nal.size(), 1, (void*)nal.data(), &dataBuf);
            if (status != VA_STATUS_SUCCESS)
            {
                OSG_WARN << "[VaapiDecoder] Failed to create slice data buffer: "
                         << vaErrorStr(status) << std::endl;
                break;
            }
            buffers.push_back(dataBuf);
        }

        bool ok = (status == VA_STATUS_SUCCESS) && !buffers.empty();
        if (ok) ok = (vaBeginPicture(_d->_vaDisplay, _d->_vaContext, cur) == VA_STATUS_SUCCESS);
        if (ok)
        {
            status = vaRenderPicture(_d->_vaDisplay, _d->_vaContext,
                                     buffers.data(), (int)buffers.size());
            ok = (status == VA_STATUS_SUCCESS);
            if (!ok)
                OSG_WARN << "[VaapiDecoder] vaRenderPicture failed: "
                         << vaErrorStr(status) << std::endl;
        }
        if (ok)
        {
            status = vaEndPicture(_d->_vaDisplay, _d->_vaContext);
            ok = (status == VA_STATUS_SUCCESS);
            if (!ok)
                OSG_WARN << "[VaapiDecoder] vaEndPicture failed: "
                         << vaErrorStr(status) << std::endl;
        }
        if (ok)
        {
            status = vaSyncSurface(_d->_vaDisplay, cur);
            ok = (status == VA_STATUS_SUCCESS);
            if (!ok)
                OSG_WARN << "[VaapiDecoder] vaSyncSurface failed: "
                         << vaErrorStr(status) << std::endl;
        }
        for (size_t i = 0; i < buffers.size(); ++i)
            vaDestroyBuffer(_d->_vaDisplay, buffers[i]);

        if (!ok) { releaseSurface(cur); return false; }

        updateDpb(first, sps, cur, first.frameNum, frameNumOffset, poc, pocMsb);
        OSG_DEBUG << "[VaapiDecoder] Decoded H.264 frame " << _frameIdx << " ("
                  << _decWidth << "x" << _decHeight << ", poc " << poc
                  << ", type " << first.sliceType
                  << (first.idr ? ", IDR" : "") << ")" << std::endl;
        _d->_vaSurface = cur;
        _d->_currentPTS = pts;
        _d->_frameReady = true;
        return true;
    }
}

/////////////////////////////////////////////////////////////////////////
// VaapiDecoder

VaapiDecoder::VaapiDecoder(int videoCodec, int width, int height)
:   _vaDisplay(nullptr), _vaConfig(VA_INVALID_ID), _vppConfig(VA_INVALID_ID), _vaContext(VA_INVALID_ID),
    _vppContext(VA_INVALID_ID), _vaSurface(VA_INVALID_SURFACE), _vaSurfaceRGB(VA_INVALID_SURFACE),
    _gbmDevice(nullptr), _gbmSurface(nullptr), _gbmBuffer(nullptr), _h264(nullptr),
    _currentPTS(0), _framebufferID(0), _videoCodec(videoCodec), _drmFD(-1), _width(width), _height(height),
    _bitDepth(8), _initialized(false), _frameReady(false), _useVPP(true)
{
    _h264 = new VaapiH264Decoder(this);
}

VaapiDecoder::~VaapiDecoder()
{
    cleanupVAAPI();
    cleanupGBM();
    delete _h264; _h264 = nullptr;
}

bool VaapiDecoder::initialize()
{
    if (_initialized) return true;
    if (!setupGBM())
    {
        OSG_WARN << "[VaapiDecoder] Failed to setup GBM" << std::endl;
        return false;
    }
    
    if (!setupVAAPI())
    {
        OSG_WARN << "[VaapiDecoder] Failed to setup VA-API" << std::endl;
        cleanupGBM(); return false;
    }
    _initialized = true; return true;
}

bool VaapiDecoder::setupGBM()
{
    _drmFD = open("/dev/dri/renderD128", O_RDWR);
    if (_drmFD < 0)
    {
        _drmFD = open("/dev/dri/card0", O_RDWR);
        if (_drmFD < 0)
        {
            OSG_WARN << "[VaapiDecoder] Failed to open DRM device" << std::endl;
            return false;
        }
    }
    return true;
}

bool VaapiDecoder::setupVAAPI()
{
    _vaDisplay = vaGetDisplayDRM(_drmFD);
    if (!_vaDisplay)
    {
        OSG_WARN << "[VaapiDecoder] Failed to get VA display" << std::endl;
        return false;
    }
    
    int majorVer = 0, minorVer = 0;
    VAStatus status = vaInitialize(_vaDisplay, &majorVer, &minorVer);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] Failed to initialize VA-API: "
                 << vaErrorStr(status) << std::endl; return false;
    }
    
    VAProfile profile = VAProfileNone;
    switch (_videoCodec)
    {
    case 0:  /**<  MPEG1             */ profile = VAProfileMPEG2Simple; break;
    case 1:  /**<  MPEG2             */ profile = VAProfileMPEG2Main; break;
    case 2:  /**<  MPEG4             */ profile = VAProfileMPEG4Main; break;
    case 3:  /**<  VC1               */ profile = VAProfileVC1Main; break;
    case 4:  /**<  H264              */ profile = VAProfileH264High; break;
    case 5:  /**<  JPEG              */ profile = VAProfileJPEGBaseline; break;
    case 8:  /**<  HEVC              */ profile = VAProfileHEVCMain; break;
    case 9:  /**<  VP8               */ profile = VAProfileVP8Version0_3; break;
    case 10: /**<  VP9               */ profile = VAProfileVP9Profile0; break;
    case 11: /**<  AV1               */ profile = VAProfileAV1Profile0; break;
    default:
        OSG_WARN << "[VaapiDecoder] Unsupported codec: " << _videoCodec << std::endl;
        return false;
    }
    
    VAConfigAttrib attrib = {};
    attrib.type = VAConfigAttribRTFormat;
    attrib.value = VA_RT_FORMAT_YUV420;
    status = vaCreateConfig(_vaDisplay, profile, VAEntrypointVLD, &attrib, 1, &_vaConfig);
    if (status != VA_STATUS_SUCCESS && _videoCodec == 4)
    {
        // Try the other H.264 profiles before giving up
        const VAProfile fallbacks[] = { VAProfileH264High, VAProfileH264Main,
                                        VAProfileH264ConstrainedBaseline };
        for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); ++i)
        {
            if (fallbacks[i] == profile) continue;
            status = vaCreateConfig(_vaDisplay, fallbacks[i], VAEntrypointVLD,
                                    &attrib, 1, &_vaConfig);
            if (status == VA_STATUS_SUCCESS) { profile = fallbacks[i]; break; }
        }
    }
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] Failed to create VA config: " << vaErrorStr(status) << std::endl;
        vaTerminate(_vaDisplay); _vaDisplay = nullptr; return false;
    }

    VAConfigAttrib attrib2 = {};
    attrib2.type = VAConfigAttribRTFormat;
    attrib2.value = VA_RT_FORMAT_RGB32;
    status = vaCreateConfig(_vaDisplay, VAProfileNone, VAEntrypointVideoProc,
                            &attrib2, 1, &_vppConfig);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] Failed to create VPP config: " << vaErrorStr(status)
                 << ", RGB conversion will be disabled" << std::endl;
        _useVPP = false;
    }
    return true;
}

bool VaapiDecoder::decode(const uint8_t* data, int size, long long pts)
{
    if (!_initialized || !data || size <= 0 || !_h264) return false;
    std::lock_guard<std::mutex> lock(_mutex);
    return _h264->decode(data, size, pts);
}

bool VaapiDecoder::convert()
{
    VABufferID buffers[1];
    if (!_initialized || !_frameReady || _vaSurface == VA_INVALID_SURFACE) return false;
    if (!_useVPP || _vppConfig == VA_INVALID_ID || _vaSurfaceRGB == VA_INVALID_SURFACE) return true;
    std::lock_guard<std::mutex> lock(_mutex);

    VAProcPipelineParameterBuffer pipelineParam;
    memset(&pipelineParam, 0, sizeof(pipelineParam));
    pipelineParam.surface = _vaSurface;
    pipelineParam.surface_region = nullptr;
    pipelineParam.output_region = nullptr;
    pipelineParam.output_background_color = 0;
    pipelineParam.filter_flags = 0;
    
    VAStatus status = vaCreateBuffer(_vaDisplay, _vppContext, VAProcPipelineParameterBufferType,
                                     sizeof(pipelineParam), 1, &pipelineParam, &buffers[0]);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] Failed to create VPP buffer: "
                 << vaErrorStr(status) << std::endl; return false;
    }

    status = vaBeginPicture(_vaDisplay, _vppContext, _vaSurfaceRGB);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] VPP BeginPicture failed: " << vaErrorStr(status) << std::endl;
        vaDestroyBuffer(_vaDisplay, buffers[0]); return false;
    }
    
    status = vaRenderPicture(_vaDisplay, _vppContext, &buffers[0], 1);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] VPP RenderPicture failed: " << vaErrorStr(status) << std::endl;
        vaEndPicture(_vaDisplay, _vppContext); vaDestroyBuffer(_vaDisplay, buffers[0]); return false;
    }

    status = vaEndPicture(_vaDisplay, _vppContext);
    if (status != VA_STATUS_SUCCESS)
    {
        //OSG_WARN << "[VaapiDecoder] VPP EndPicture failed: " << vaErrorStr(status) << std::endl;
        //vaDestroyBuffer(_vaDisplay, buffers[0]); return false;
    }

    syncSurface(false); vaDestroyBuffer(_vaDisplay, buffers[0]);
    _frameReady = false;
    return true;
}

bool VaapiDecoder::syncSurface(bool origin)
{
    VASurfaceID surface = (origin ? _vaSurface : _vaSurfaceRGB);
    if (!_initialized || surface == VA_INVALID_SURFACE) return false;

    VAStatus status = vaSyncSurface(_vaDisplay, surface);
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] vaSyncSurface failed: "
                 << vaErrorStr(status) << std::endl; return false;
    }
    return true;
}

bool VaapiDecoder::exportDecodedFrame(int& fourcc, std::vector<ExportedBufferData>& bufferData)
{
    VASurfaceID surface = (!_useVPP ? _vaSurface : _vaSurfaceRGB);
    if (!_initialized || surface == VA_INVALID_SURFACE) return false;
    std::lock_guard<std::mutex> lock(_mutex);

    VADRMPRIMESurfaceDescriptor desc = {};
    VAStatus status = vaExportSurfaceHandle(
        _vaDisplay, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
    if (status != VA_STATUS_SUCCESS)
    {
        status = vaExportSurfaceHandle(
            _vaDisplay, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME,
            VA_EXPORT_SURFACE_READ_ONLY, &desc);
    }
    
    if (status != VA_STATUS_SUCCESS)
    {
        OSG_WARN << "[VaapiDecoder] vaExportSurfaceHandle failed: "
                 << vaErrorStr(status) << std::endl; return false;
    }
    else if (desc.num_objects < 1 || desc.num_layers < 1)
    {
        OSG_WARN << "[VaapiDecoder] Invalid export descriptor" << std::endl;
        return false;
    }
    
    for (uint32_t i = 0; i < desc.num_layers; i++)
    {
        for (uint32_t j = 0; j < desc.layers[i].num_planes; j++)
        {
            uint32_t obj_idx = desc.layers[i].object_index[j];
            if (obj_idx < desc.num_objects)
            {
                ExportedBufferData exported = {};
                exported.fd = dup(desc.objects[obj_idx].fd);
                exported.drmPrimePitch = desc.layers[i].pitch[j];
                exported.drmPrimeOffset = desc.layers[i].offset[j];
                exported.drmPrimeModifier = desc.objects[obj_idx].drm_format_modifier;
                bufferData.push_back(exported);
            }
        }
    }
    
    for (uint32_t i = 0; i < desc.num_objects; i++)
    { if (desc.objects[i].fd >= 0) close(desc.objects[i].fd); }
    fourcc = toDRMFourcc(desc.fourcc); return !bufferData.empty();
}

void VaapiDecoder::releaseFrame()
{
    _frameReady = false;
}

void VaapiDecoder::cleanupVAAPI()
{
    // Release everything with a valid display only: initialize() may call this after a failure
    // and the destructor calls it again, where _vaDisplay is already NULL
    if (_vaDisplay == nullptr) return;

    if (_vppContext != VA_INVALID_ID)
    { vaDestroyContext(_vaDisplay, _vppContext); _vppContext = VA_INVALID_ID; }
    if (_vaContext != VA_INVALID_ID)
    { vaDestroyContext(_vaDisplay, _vaContext); _vaContext = VA_INVALID_ID; }
    if (_h264) _h264->releaseSurfaces();

    if (_vaSurfaceRGB != VA_INVALID_SURFACE)
    { vaDestroySurfaces(_vaDisplay, &_vaSurfaceRGB, 1); _vaSurfaceRGB = VA_INVALID_SURFACE; }
    _vaSurface = VA_INVALID_SURFACE;
    
    if (_vppConfig != VA_INVALID_ID)
    { vaDestroyConfig(_vaDisplay, _vppConfig); _vppConfig = VA_INVALID_ID; }
    if (_vaConfig != VA_INVALID_ID)
    { vaDestroyConfig(_vaDisplay, _vaConfig); _vaConfig = VA_INVALID_ID; }
    
    vaTerminate(_vaDisplay); _vaDisplay = nullptr;
}

void VaapiDecoder::cleanupGBM()
{
    if (_drmFD >= 0) { close(_drmFD); _drmFD = -1; }
}
