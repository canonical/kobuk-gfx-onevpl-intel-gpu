// Copyright (c) 2009-2022 Intel Corporation
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
#include "mfx_common.h"
#ifdef MFX_ENABLE_H264_VIDEO_ENCODE

#include <vector>
#include <list>
#include <memory>
#include <algorithm> /* for std::find_if on Linux/Android */
#include <mfx_brc_common.h>

#include "mfx_h264_encode_struct_vaapi.h"
#include "umc_mutex.h"
#include "umc_event.h"
#include "umc_h264_brc.h"
#include "mfx_h264_enc_common_hw.h"
#include "mfx_ext_buffers.h"
#include "mfx_h264_encode_interface.h"
#ifdef MFX_ENABLE_EXT
#include "mfx_h264_encode_cm.h"
#ifdef MFX_ENABLE_ASC
#include "asc_cm.h"
#endif
#else
#include "asc.h"
#endif
#include "ippi.h"
#include "libmfx_core_interface.h"

#include "mfx_vpp_helper.h"
#include "mfx_platform_caps.h"

#ifdef MFX_ENABLE_MCTF_IN_AVC
#include "cmvm.h"
#include "mctf_common.h"
#endif

#ifndef _MFX_H264_ENCODE_HW_UTILS_H_
#define _MFX_H264_ENCODE_HW_UTILS_H_

#include <unordered_map>
#include <queue>
#include <mutex>

inline constexpr
bool bIntRateControlLA(mfxU16 mode)
{
    return
           (mode == MFX_RATECONTROL_LA)
        || (mode == MFX_RATECONTROL_LA_ICQ)
        || (mode == MFX_RATECONTROL_LA_HRD)
        ;
}

inline constexpr
bool bRateControlLA(mfxU16 mode)
{
    return bIntRateControlLA(mode);
}

#define MFX_H264ENC_HW_TASK_TIMEOUT 2000
#define MFX_H264ENC_HW_TASK_TIMEOUT_SIM 600000

#define MFX_ARRAY_SIZE(ARR) (sizeof(ARR)/sizeof(ARR[0]))
const int MFX_MAX_DIRTY_RECT_COUNT = MFX_ARRAY_SIZE(mfxExtDirtyRect::Rect);
const int MFX_MAX_MOVE_RECT_COUNT = MFX_ARRAY_SIZE(mfxExtMoveRect::Rect);
const int DEFAULT_PPYR_INTERVAL = 3;

namespace MfxHwH264Encode
{
    struct VmeData;

    enum {
        STAGE_QP   = 0xf00000,
        STAGE_ENC1 = 0x0f0000,
        STAGE_PAK1 = 0x00f000,
        STAGE_ENC2 = 0x000f00,
        STAGE_PAK2 = 0x0000f0,
        STAGE_ENC  = STAGE_ENC1 | STAGE_ENC2,
        STAGE_PAK  = STAGE_PAK1 | STAGE_PAK2,
        STAGE_ALL  = 0xffffff
    };

    static const mfxU16 MFX_MEMTYPE_SYS_INT =
        MFX_MEMTYPE_FROM_ENCODE | MFX_MEMTYPE_SYSTEM_MEMORY | MFX_MEMTYPE_INTERNAL_FRAME;

    static const mfxU16 MFX_MEMTYPE_SYS_EXT =
        MFX_MEMTYPE_FROM_ENCODE | MFX_MEMTYPE_SYSTEM_MEMORY | MFX_MEMTYPE_EXTERNAL_FRAME;

    static const mfxU16 MFX_MEMTYPE_D3D_INT =
        MFX_MEMTYPE_FROM_ENCODE | MFX_MEMTYPE_DXVA2_DECODER_TARGET | MFX_MEMTYPE_INTERNAL_FRAME;

    static const mfxU16 MFX_MEMTYPE_D3D_EXT =
        MFX_MEMTYPE_FROM_ENCODE | MFX_MEMTYPE_DXVA2_DECODER_TARGET | MFX_MEMTYPE_EXTERNAL_FRAME;


    mfxU16 CalcNumFrameMin(const MfxHwH264Encode::MfxVideoParam &par, MFX_ENCODE_CAPS const & hwCaps, eMFXHWType platform);

    enum
    {
        TFIELD = 0,
        BFIELD = 1
    };

    enum
    {
        ENC = 0,
        DISP = 1
    };

    /*
        NAL unit types for internal usage
    */
    enum {
        NALU_NON_IDR = 1,
        NALU_IDR = 5,
        NALU_SEI = 6,
        NALU_SPS = 7,
        NALU_PPS = 8,
        NALU_AUD = 9,
        NALU_PREFIX = 14,
        NALU_CODED_SLICE_EXT = 20,
    };


    class DdiTask;

    struct mfxExtAvcSeiBufferingPeriod
    {
        mfxU8  seq_parameter_set_id;
        mfxU8  nal_cpb_cnt;
        mfxU8  vcl_cpb_cnt;
        mfxU8  initial_cpb_removal_delay_length;
        mfxU32 nal_initial_cpb_removal_delay[32];
        mfxU32 nal_initial_cpb_removal_delay_offset[32];
        mfxU32 vcl_initial_cpb_removal_delay[32];
        mfxU32 vcl_initial_cpb_removal_delay_offset[32];
    };

    struct mfxExtAvcSeiPicTiming
    {
        mfxU8  cpb_dpb_delays_present_flag;
        mfxU8  cpb_removal_delay_length;
        mfxU8  dpb_output_delay_length;
        mfxU8  pic_struct_present_flag;
        mfxU8  time_offset_length;

        mfxU32 cpb_removal_delay;
        mfxU32 dpb_output_delay;
        mfxU8  pic_struct;
        mfxU8  ct_type;
    };

    struct mfxExtAvcSeiDecRefPicMrkRep
    {
        mfxU8  original_idr_flag;
        mfxU16 original_frame_num;
        mfxU8  original_field_info_present_flag;
        mfxU8  original_field_pic_flag;
        mfxU8  original_bottom_field_flag;
        mfxU8  no_output_of_prior_pics_flag;
        mfxU8  long_term_reference_flag;
        mfxU8  adaptive_ref_pic_marking_mode_flag;
        mfxU32 num_mmco_entries;                // number of currently valid mmco, value pairs
        mfxU8  mmco[32];                        // memory management control operation id
        mfxU32 value[64];                       // operation-dependent data, max 2 per operation
    };

    struct mfxExtAvcSeiRecPoint
    {
        mfxU16 recovery_frame_cnt;
        mfxU8  exact_match_flag;
        mfxU8  broken_link_flag;
        mfxU8  changing_slice_group_idc;
    };

#ifdef MFX_ENABLE_AVC_CUSTOM_QMATRIX
    struct unifiedQMatrixH264
    {
        mfxU8  scalingList4x4[6][16];
        mfxU8  scalingList8x8[2][64];
    };
#endif

    template <typename T> struct Pair
    {
        T top;
        T bot;

        Pair()
            : top()
            , bot()
        {
        }

        template<typename U> Pair(Pair<U> const & pair)
            : top(static_cast<T>(pair.top))
            , bot(static_cast<T>(pair.bot))
        {
        }

        template<typename U> explicit Pair(U const & value)
            : top(static_cast<T>(value))
            , bot(static_cast<T>(value))
        {
        }

        template<typename U> Pair(U const & t, U const & b)
            : top(static_cast<T>(t))
            , bot(static_cast<T>(b))
        {
        }

        template<typename U>
        Pair<T> & operator =(Pair<U> const & pair)
        {
            Pair<T> tmp(pair);
            std::swap(*this, tmp);
            return *this;
        }

        T & operator[] (mfxU32 parity)
        {
            assert(parity < 2);
            return (&top)[parity & 1];
        }

        T const & operator[] (mfxU32 parity) const
        {
            assert(parity < 2);
            return (&top)[parity & 1];
        }
    };

    template <class T> Pair<T> MakePair(T const & top, T const & bot)
    {
        return Pair<T>(top, bot);
    }

    template <class T> bool operator ==(Pair<T> const & l, Pair<T> const & r)
    {
        return l.top == r.top && l.bot == r.bot;
    }

    typedef Pair<mfxU8>  PairU8;
    typedef Pair<mfxI8>  PairI8;
    typedef Pair<mfxU16> PairU16;
    typedef Pair<mfxU32> PairU32;
    typedef Pair<mfxI32> PairI32;

    void PrepareSeiMessage(
        DdiTask const &               task,
        mfxU32                        nalHrdBpPresentFlag,
        mfxU32                        vclHrdBpPresentFlag,
        mfxU32                        seqParameterSetId,
        mfxExtAvcSeiBufferingPeriod & msg);

    void PrepareSeiMessage(
        DdiTask const &                task,
        mfxU32                         fieldId,
        mfxU32                         cpbDpbDelaysPresentFlag,
        mfxU32                         picStructPresentFlag,
        mfxExtAvcSeiPicTiming &        msg);

    void PrepareSeiMessage(
        DdiTask const &               task,
        mfxU32                        fieldId,
        mfxU32                        frame_mbs_only_flag,
        mfxExtAvcSeiDecRefPicMrkRep & msg);

    void PrepareSeiMessage(
        MfxVideoParam const &   par,
        mfxExtAvcSeiRecPoint &  msg,
        mfxU16 recovery_frame_cnt);

    mfxU32 CalculateSeiSize( mfxExtAvcSeiRecPoint const & msg);
    mfxU32 CalculateSeiSize( mfxExtAvcSeiDecRefPicMrkRep const & msg);

// MVC BD {
    mfxU32 CalculateSeiSize( mfxExtAvcSeiBufferingPeriod const & msg);
    mfxU32 CalculateSeiSize(
        mfxExtPictureTimingSEI const & extPt,
        mfxExtAvcSeiPicTiming const & msg);
// MVC BD }

    mfxStatus CheckBeforeCopy(
        mfxExtMVCSeqDesc &       dst,
        mfxExtMVCSeqDesc const & src);

    mfxStatus CheckBeforeCopyQueryLike(
        mfxExtMVCSeqDesc &       dst,
        mfxExtMVCSeqDesc const & src);

    void Copy(
        mfxExtMVCSeqDesc &       dst,
        mfxExtMVCSeqDesc const & src);

    void FastCopyBufferVid2Sys(
        void *       dstSys,
        void const * srcVid,
        mfxI32       bytes);

    void FastCopyBufferSys2Vid(
        void *       dstSys,
        void const * srcVid,
        mfxI32       bytes);

    PairU8 ExtendFrameType(
        mfxU32 type);

    mfxU8 CalcTemporalLayerIndex(
        MfxVideoParam const & video,
        mfxI32                frameOrder);

    bool CheckSubMbPartition(
        mfxExtCodingOptionDDI const * extDdi,
        mfxU8                         frameType);

    mfxU8 GetPFrameLevel(
        mfxU32 i,
        mfxU32 num);

    mfxU8 PLayer(
        MfxVideoParam const & par,
        mfxU32                order);

    mfxU8 GetQpValue(
        DdiTask const &       task,
        MfxVideoParam const & par,
        mfxU32                frameType);

    PairU16 GetPicStruct(
        MfxVideoParam const & video,
        mfxU16                runtPs);

    PairU16 GetPicStruct(
        MfxVideoParam const & video,
        DdiTask const &       task);

    bool isBitstreamUpdateRequired(MfxVideoParam const & video,
        MFX_ENCODE_CAPS caps,
        eMFXHWType platform);

    enum
    {
        H264_FRAME_FLAG_SKIPPED = 1,
        H264_FRAME_FLAG_READY = 2
    };

    class MfxFrameAllocResponse : public mfxFrameAllocResponse
    {
    public:
        MfxFrameAllocResponse();

        ~MfxFrameAllocResponse();

        mfxStatus Alloc(
            VideoCORE *            core,
            mfxFrameAllocRequest & req,
            bool isCopyRequired = true,
            bool isAllFramesRequired = false);
#ifdef MFX_ENABLE_EXT
        mfxStatus AllocCmBuffers(
            CmDevice *             device,
            mfxFrameAllocRequest & req);

        mfxStatus AllocCmBuffersUp(
            CmDevice *             device,
            mfxFrameAllocRequest & req);

        mfxStatus AllocCmSurfaces(
            CmDevice *             device,
            mfxFrameAllocRequest & req);

        mfxStatus AllocCmSurfacesUP(
            CmDevice *             device,
            mfxFrameAllocRequest & req);
#endif
        mfxStatus AllocFrames(
            VideoCORE *            core,
            mfxFrameAllocRequest & req);
        mfxStatus UpdateResourcePointers(
            mfxU32                 idxScd,
            void *                 memY,
            void *                 gpuSurf);

        void * GetSysmemBuffer(mfxU32 idx);

        mfxU32 Lock(mfxU32 idx);

        void Unlock();

        mfxU32 Unlock(mfxU32 idx);

        mfxU32 Locked(mfxU32 idx) const;

        void   ClearFlag(mfxU32 idx);
        void   SetFlag(mfxU32 idx, mfxU32 flag);
        mfxU32 GetFlag(mfxU32 idx) const;
    private:
        MfxFrameAllocResponse(MfxFrameAllocResponse const &);
        MfxFrameAllocResponse & operator =(MfxFrameAllocResponse const &);
#ifdef MFX_ENABLE_EXT
        static void DestroyBuffer     (CmDevice * device, void * p);
        static void DestroySurface    (CmDevice * device, void * p);
        static void DestroySurface2DUP(CmDevice * device, void * p);
        static void DestroyBufferUp   (CmDevice * device, void * p);
        void (*m_cmDestroy)(CmDevice *, void *) = 0;

        CmDevice *  m_cmDevice = nullptr;
#endif
        VideoCORE * m_core;
        mfxU16      m_numFrameActualReturnedByAllocFrames;

        std::vector<mfxFrameAllocResponse> m_responseQueue;
        std::vector<mfxMemId>              m_mids;
        std::vector<mfxU32>                m_locked;
        std::vector<mfxU32>                m_flag;
        std::vector<void *>                m_sysmems;
    };

    mfxU32 FindFreeResourceIndex(
        MfxFrameAllocResponse const & pool,
        mfxU32                        startingFrom = 0);

    mfxMemId AcquireResource(
        MfxFrameAllocResponse & pool,
        mfxU32                  index);

    mfxMemId AcquireResource(
        MfxFrameAllocResponse & pool);

    mfxHDLPair AcquireResourceUp(
        MfxFrameAllocResponse & pool,
        mfxU32                  index);

    mfxHDLPair AcquireResourceUp(
        MfxFrameAllocResponse & pool);

    void ReleaseResource(
        MfxFrameAllocResponse & pool,
        mfxMemId                mid);

    // add hwType param
    mfxStatus CheckEncodeFrameParam(
        MfxVideoParam const &     video,
        mfxEncodeCtrl *           ctrl,
        mfxFrameSurface1 *        surface,
        mfxBitstream *            bs,
        bool                      isExternalFrameAllocator,
        MFX_ENCODE_CAPS const &   caps);

    template<typename T> void Clear(std::vector<T> & v)
    {
        std::vector<T>().swap(v);
    }


    template<class T, mfxU32 N>
    struct FixedArray
    {
        FixedArray()
            : m_numElem(0)
        {
        }

        explicit FixedArray(T fillVal)
            : m_numElem(0)
        {
            Fill(fillVal);
        }

        void PushBack(T const & val)
        {
            assert(m_numElem < N);
            m_arr[m_numElem] = val;
            m_numElem++;
        }

        void PushFront(T const val)
        {
            assert(m_numElem < N);
            std::copy(m_arr, m_arr + m_numElem, m_arr + 1);
            m_arr[0] = val;
            m_numElem++;
        }

        void Erase(T * p)
        {
            assert(p >= m_arr && p <= m_arr + m_numElem);

            m_numElem = mfxU32(
                std::copy(p + 1, m_arr + m_numElem, p) - m_arr);
        }

        void Erase(T * b, T * e)
        {
            assert(b <= e);
            assert(b >= m_arr && b <= m_arr + m_numElem);
            assert(e >= m_arr && e <= m_arr + m_numElem);

            m_numElem = mfxU32(
                std::copy(e, m_arr + m_numElem, b) - m_arr);
        }

        void Resize(mfxU32 size, T fillVal = T())
        {
            assert(size <= N);
            for (mfxU32 i = m_numElem ; i < size; ++i)
                m_arr[i] = fillVal;
            m_numElem = size;
        }

        T * Begin()
        {
            return m_arr;
        }

        T const * Begin() const
        {
            return m_arr;
        }

        T * End()
        {
            return m_arr + m_numElem;
        }

        T const * End() const
        {
            return m_arr + m_numElem;
        }

        T & Back()
        {
            assert(m_numElem > 0);
            return m_arr[m_numElem - 1];
        }

        T const & Back() const
        {
            assert(m_numElem > 0);
            return m_arr[m_numElem - 1];
        }

        mfxU32 Size() const
        {
            return m_numElem;
        }

        mfxU32 Capacity() const
        {
            return N;
        }

        T & operator[](mfxU32 idx)
        {
            assert(idx < N);
            return m_arr[idx];
        }

        T const & operator[](mfxU32 idx) const
        {
            assert(idx < N);
            return m_arr[idx];
        }

        void Fill(T val)
        {
            for (mfxU32 i = 0; i < N; i++)
            {
                m_arr[i] = val;
            }
        }

        template<mfxU32 M>
        bool operator==(const FixedArray<T, M>& r) const
        {
            assert(Size() <= N);
            assert(r.Size() <= M);

            if (Size() != r.Size())
            {
                return false;
            }

            for (mfxU32 i = 0; i < Size(); i++)
            {
                if (m_arr[i] != r[i])
                {
                    return false;
                }
            }

            return true;
        }

    private:
        T      m_arr[N]  = {};
        mfxU32 m_numElem = 0;
    };

    struct BiFrameLocation
    {
        BiFrameLocation() { Zero(*this); }

        mfxU32 miniGopCount;    // sequence of B frames between I/P frames
        mfxU32 encodingOrder;   // number within mini-GOP (in encoding order)
        mfxU16 refFrameFlag;    // MFX_FRAMETYPE_REF if B frame is reference
        mfxU32 level;           // level of pyramid
    };

    class FrameTypeGenerator
    {
    public:
        FrameTypeGenerator();

        void Init(MfxVideoParam const & video);

        PairU8 Get() const;

        BiFrameLocation GetBiFrameLocation() const;

        void Next();

    private:
        mfxU32 m_frameOrder;    // in display order
        mfxU16 m_gopOptFlag;
        mfxU16 m_gopPicSize;
        mfxU16 m_gopRefDist;
        mfxU16 m_refBaseDist;   // key picture distance
        mfxU16 m_biPyramid;
        mfxU32 m_idrDist;
    };

    /* Intra refresh types */
    enum {
        MFX_REFRESH_NO = 0,
        MFX_REFRESH_VERTICAL = 1,
        MFX_REFRESH_HORIZONTAL = 2,
        MFX_REFRESH_SLICE = 3
    };

    struct IntraRefreshState
    {
        IntraRefreshState() : refrType(0), IntraLocation(0), IntraSize(0), IntRefQPDelta(0), firstFrameInCycle(false) {}
        bool operator==(const IntraRefreshState &b) const
        {
            return (refrType == b.refrType &&
                    IntraLocation == b.IntraLocation &&
                    IntraSize == b.IntraSize &&
                    IntRefQPDelta == b.IntRefQPDelta &&
                    firstFrameInCycle == b.firstFrameInCycle);
        }
        bool operator!=(const IntraRefreshState &b) const
        {
            return !(*this == b);
        }

        mfxU16  refrType;
        mfxU16  IntraLocation;
        mfxU16  IntraSize;
        mfxI16  IntRefQPDelta;
        bool    firstFrameInCycle;
    };

    class Surface
    {
    public:
        Surface()
            : m_free(true)
        {
        }

        bool IsFree() const
        {
            return m_free;
        }

        void SetFree(bool free)
        {
            m_free = free;
        }

    private:
        bool m_free;
    };

    class Reconstruct : public Surface
    {
    public:
        Reconstruct()
            : Surface()
            , m_yuv(0)
            , m_frameOrderIdrInDisplayOrder(0)
            , m_frameOrderIdr(0)
            , m_frameOrderI(0)
            , m_frameOrder(0)
            , m_baseLayerOrder(0)
            , m_frameOrderStartTScalStructure(0)
            , m_frameNum(0)
            , m_frameNumWrap(0)
            , m_picNum(0, 0)
            , m_longTermFrameIdx(NO_INDEX_U8)
            , m_longTermPicNum(NO_INDEX_U8, NO_INDEX_U8)
            , m_reference(false, false)
            , m_picStruct((mfxU16)MFX_PICSTRUCT_PROGRESSIVE)
            , m_extFrameTag(0)
            , m_tid(0)
            , m_tidx(0)
            , m_panicMode(0)
        {
        }

        mfxU16 GetPicStructForEncode() const
        {
            return m_picStruct[ENC];
        }

        mfxU16 GetPicStructForDisplay() const
        {
            return m_picStruct[DISP];
        }

        mfxU8 GetFirstField() const
        {
            return (GetPicStructForEncode() & MFX_PICSTRUCT_FIELD_BFF) ? 1 : 0;
        }

        mfxI32 GetPoc(mfxU32 parity) const
        {
            return 2 * ((m_frameOrder - m_frameOrderIdr) & 0x7fffffff) + (parity != GetFirstField());
        }

        PairI32 GetPoc() const
        {
            return PairI32(GetPoc(TFIELD), GetPoc(BFIELD));
        }

        mfxU32 GetPictureCount() const
        {
            return (GetPicStructForEncode() & MFX_PICSTRUCT_PROGRESSIVE) ? 1 : 2;
        }

        mfxFrameSurface1* m_yuv;
        mfxU32  m_frameOrderIdrInDisplayOrder; // most recent idr frame in display order
        mfxU32  m_frameOrderIdr;    // most recent idr frame in encoding order
        mfxU32  m_frameOrderI;      // most recent i frame in encoding order
        mfxU32  m_frameOrder;
        mfxU32  m_baseLayerOrder;
        mfxU32  m_frameOrderStartTScalStructure; // starting point of temporal scalability structure
        mfxU16  m_frameNum;
        mfxI32  m_frameNumWrap;
        PairI32 m_picNum;
        mfxU8   m_longTermFrameIdx;
        PairU8  m_longTermPicNum;
        PairU8  m_reference;        // is refrence (short or long term) or not
        PairU16 m_picStruct;
        mfxU32  m_extFrameTag;
        mfxU32  m_tid;              // temporal_id
        mfxU8  m_tidx;             // temporal layer index (in acsending order of temporal_id)
        mfxU8   m_panicMode;
    };

    struct RefListMod
    {
        RefListMod() : m_idc(3), m_diff(0) {}
        RefListMod(mfxU16 idc, mfxU16 diff) : m_idc(idc), m_diff(diff) { assert(idc < 6); }
        mfxU16 m_idc;
        mfxU16 m_diff;
    };

    struct WeightTab
    {
        mfxU8 m_lumaWeightL0Flag:1;
        mfxU8 m_chromaWeightL0Flag:1;
        mfxU8 m_lumaWeightL1Flag:1;
        mfxU8 m_chromaWeightL1Flag:1;

        mfxI8 m_lumaWeightL0;
        mfxI8 m_lumaOffsetL0;
        mfxI8 m_lumaWeightL1;
        mfxI8 m_lumaOffsetL1;
        mfxI8 m_chromaWeightL0[2];
        mfxI8 m_chromaOffsetL0[2];
        mfxI8 m_chromaWeightL1[2];
        mfxI8 m_chromaOffsetL1[2];
    };

    struct DpbFrame
    {
        DpbFrame() { Zero(*this); }

        PairI32 m_poc;
        mfxU32  m_frameOrder;
        mfxU32  m_extFrameTag; // frameOrder assigned by application
        mfxU32  m_frameNum;
        mfxI32  m_frameNumWrap;
        mfxI32  m_picNum[2];
        mfxU32  m_viewIdx;
        mfxU32  m_frameIdx;
        mfxU32  m_tid;
        PairU8  m_longTermPicNum;
        PairU8  m_refPicFlag;
        mfxU8   m_longTermIdxPlus1;
        mfxU8   m_longterm; // at least one field is a long term reference
        mfxU8   m_refBase;
        mfxU8   m_keyRef;   //short term reference stored in DPB as long as possible

        mfxU8   m_PIFieldFlag; // for P/I field pair

        mfxMemId        m_midRec;
#ifdef MFX_ENABLE_EXT
        CmSurface2D *   m_cmRaw;
        CmSurface2D *   m_cmRawLa;
        CmBufferUP *    m_cmMb;
#endif
#ifdef MFX_ENABLE_FADE_DETECTION
        CmBufferUP *  m_cmHist;
        void *        m_cmHistSys;
#endif
        mfxMemId          m_midRaw; // for RefRaw mode
        mfxFrameSurface1* m_yuvRaw; // for RefRaw mode
    };

    inline bool operator ==(DpbFrame const & l, DpbFrame const & r)
    {
        return l.m_frameIdx == r.m_frameIdx && l.m_poc == r.m_poc && l.m_viewIdx == r.m_viewIdx;
    }

    typedef FixedArray<mfxU32, 16>     ArrayU32x16;
    typedef FixedArray<mfxU32, 64>     ArrayU32x64;
    typedef FixedArray<mfxU16, 16>     ArrayU16x16;
    typedef FixedArray<mfxU8, 8>       ArrayU8x8;
    typedef FixedArray<mfxU8, 16>      ArrayU8x16;
    typedef FixedArray<mfxU8, 32>      ArrayU8x32;
    typedef FixedArray<mfxU8, 33>      ArrayU8x33;
    typedef FixedArray<PairU32, 16>    ArrayPairU32x16;
    typedef FixedArray<RefListMod, 32> ArrayRefListMod;

    typedef FixedArray<mfxRoiDesc, 256>                               ArrayRoi;
    typedef FixedArray<mfxRectDesc, MFX_MAX_DIRTY_RECT_COUNT>         ArrayRect;
    typedef FixedArray<mfxMovingRectDesc, MFX_MAX_MOVE_RECT_COUNT>    ArrayMovingRect;

    struct ArrayDpbFrame : public FixedArray<DpbFrame, 16>
    {
        ArrayDpbFrame()
        : FixedArray<DpbFrame, 16>()
        {
            m_maxLongTermFrameIdxPlus1.Resize(8, 0);
        }

        ArrayU8x8 m_maxLongTermFrameIdxPlus1; // for each temporal layer
    };

    struct DecRefPicMarkingInfo
    {
        DecRefPicMarkingInfo() { Zero(*this); }

        void PushBack(mfxU8 op, mfxU32 param0, mfxU32 param1 = 0)
        {
            mmco.PushBack(op);
            value.PushBack(param0);
            value.PushBack(param1);
        }

        void Clear()
        {
            mmco.Resize(0);
            value.Resize(0);
            value.Resize(0);
        }

        mfxU8       no_output_of_prior_pics_flag;
        mfxU8       long_term_reference_flag;
        ArrayU8x32  mmco;       // memory management control operation id
        ArrayU32x64 value;      // operation-dependent data, max 2 per operation
    };

    struct DecRefPicMarkingRepeatInfo
    {
        DecRefPicMarkingRepeatInfo()
            : presentFlag(false)
            , original_idr_flag(0)
            , original_frame_num(0)
            , original_field_pic_flag(0)
            , original_bottom_field_flag(0)
            , dec_ref_pic_marking()
        {
        }

        bool                 presentFlag;
        mfxU8                original_idr_flag;
        mfxU16               original_frame_num;
        mfxU8                original_field_pic_flag;
        mfxU8                original_bottom_field_flag;
        DecRefPicMarkingInfo dec_ref_pic_marking;
    };
    struct SliceStructInfo
    {
        mfxU32 startMB;
        mfxU32 numMB;
        mfxF32 weight;
        mfxU32 cost;
    };
#if defined(MFX_ENABLE_EXT_BRC)
    struct BRCFrameParams : mfxBRCFrameParam
    {
        mfxU16 picStruct;
        mfxU32 OptimalFrameSizeInBytes;
        mfxU32 LaAvgEncodedSize;
        mfxU32 LaCurEncodedSize;
        mfxU16 LaIDist;
    };
#else
    struct BRCFrameParams
    {
        mfxU32 reserved[23];
        mfxU16 SceneChange;     // Frame is Scene Chg frame
        mfxU16 LongTerm;        // Frame is long term refrence
        mfxU32 FrameCmplx;      // Frame Complexity
        mfxU32 EncodedOrder;    // Frame number in a sequence of reordered frames starting from encoder Init()
        mfxU32 DisplayOrder;    // Frame number in a sequence of frames in display order starting from last IDR
        mfxU32 CodedFrameSize;  // Size of frame in bytes after encoding
        mfxU16 FrameType;       // See FrameType enumerator
        mfxU16 PyramidLayer;    // B-pyramid or P-pyramid layer, frame belongs to
        mfxU16 NumRecode;       // Number of recodings performed for this frame
        mfxU16 NumExtParam;
        mfxExtBuffer** ExtParam;
        mfxU16 picStruct;
        mfxU32 OptimalFrameSizeInBytes;
        mfxU32 optimalBufferFullness;
    };
#endif


    class DdiTask : public Reconstruct
    {
    public:
        DdiTask()
            : Reconstruct()
            , m_pushed(0)
            , m_type(0, 0)
            , m_QPdelta(0)
            , m_bQPDelta(false)
            , m_QPmodulation(0)
            , m_currGopRefDist(0)
            , m_dpb()
            , m_internalListCtrlPresent(false)
            , m_internalListCtrlHasPriority(true)
            , m_internalListCtrlRefModLTR(false)
            , m_keyReference(false)
            , m_initCpbRemoval(0)
            , m_initCpbRemovalOffset(0)
            , m_cpbRemoval(0, 0)
            , m_dpbOutputDelay(0)
            , m_encOrder(mfxU32(-1))
            , m_encOrderIdr(0)
            , m_encOrderI(0)

            , m_viewIdx(0)
            , m_idx(NO_INDEX)
            , m_idxBs(NO_INDEX, NO_INDEX)
            , m_idxBsOffset(0)
            , m_idxRecon(NO_INDEX)
            , m_idxInterLayerRecon(0)
            , m_idxReconOffset(0)
            , m_idrPicId(mfxU16(-1))
            , m_subMbPartitionAllowed(0, 0)
            , m_cqpValue(0, 0)
            , m_insertAud(0, 0)
            , m_insertSps(0, 0)
            , m_insertPps(0, 0)
            , m_AUStartsFromSlice(0, 0)
            , m_nalRefIdc(0, 0)
            , m_statusReportNumber(0, 0)

            , m_pid(0)
            , m_fillerSize(0, 0)
            , m_addRepackSize(0, 0)
            , m_maxIFrameSize(0)
            , m_maxPBFrameSize(0)
            , m_numMbPerSlice(0)
            , m_numSlice(0, 0)
            , m_numRoi(0)
            , m_NumDeltaQpForNonRectROI(0)
            , m_roiMode(MFX_ROI_MODE_PRIORITY)
            , m_numDirtyRect(0)
            , m_numMovingRect(0)
            , m_did(0)
            , m_qid(0)
            , m_storeRefBasePicFlag(0)

            , m_bs(0)
#if defined(MFX_ENABLE_ENCODE_QUALITYINFO)
            , m_qualityInfoMode(0)
            , m_qualityInfoOutput(0)
#endif
            , m_bsDataLength(0, 0)
            , m_numLeadingFF(0, 0)
            , m_qpY(0, 0)
            , m_mad(0, 0)
            , m_minFrameSize (0)
#ifdef MFX_ENABLE_ENCODE_STATS
            , m_encodeStats(0)
#endif
            , m_notProtected(false)
            , m_nextLayerTask(0)
            , m_repack(0)
            , m_fractionalQP(0)
#ifdef MFX_ENABLE_APQ_LQ
            , m_ALQOffset(0)
#endif
            , m_midRaw(MID_INVALID)
            , m_midRec(MID_INVALID)
            , m_midBit(mfxMemId(MID_INVALID))
#if defined(MFX_ENABLE_MCTF_IN_AVC)
            , m_midMCTF(mfxMemId(MID_INVALID))
            , m_idxMCTF(NO_INDEX)
            , m_cmMCTF(0)
#endif
#ifdef MFX_ENABLE_EXT
            , m_cmRaw(0)
            , m_cmRawLa(0)
            , m_cmMb(0)
            , m_cmMbSys(0)
            , m_cmRefMb(0)
            , m_cmCurbe(0)
            , m_cmRefs(0)
            , m_cmRefsLa(0)
            , m_event(0)
#endif
            , m_vmeData(0)
            , m_fwdRef(0)
            , m_bwdRef(0)
            , m_adaptiveTUEnabled(0)
            , m_fieldPicFlag(0)
            , m_singleFieldMode(false)
            , m_fieldCounter(0)
            , m_timeStamp(0)
            , m_minQP(0)
            , m_maxQP(0)
            , m_resetBRC(false)
            , m_idxMBQP{ NO_INDEX, NO_INDEX }
            , m_midMBQP{ MID_INVALID, MID_INVALID }
            , m_isMBQP{ false, false }
            , m_isUseRawRef(false)
            , m_isSkipped (false)
            , m_toRecode (false)
            , m_isMBControl(false)
            , m_midMBControl(MID_INVALID)
            , m_idxMBControl(NO_INDEX)
#if defined(MFX_ENABLE_AVC_CUSTOM_QMATRIX)
            , m_adaptiveCQMHint(CQM_HINT_INVALID)
#endif
#ifdef MFX_ENABLE_FADE_DETECTION
            , m_cmRawForHist(0)
            , m_cmHist(0)
            , m_cmHistSys(0)
#endif
            , m_isENCPAK(false)
            , m_startTime(0)
            , m_hwType(MFX_HW_UNKNOWN)
#if defined(MFX_ENABLE_H264_REPARTITION_CHECK)
            , m_RepartitionCheck(0)
#endif
            , m_TCBRCTargetFrameSize(0)
            , m_SceneChange(0)
#if defined(MFX_ENABLE_ENCTOOLS)
            , m_SpatialComplexity(0)
            , m_PersistenceMapNZ(0)
            , m_PersistenceMap{}
#endif
#if defined(MFX_ENABLE_MCTF_IN_AVC)
            , m_doMCTFIntraFiltering(0)
#endif
            , m_LowDelayPyramidLayer(0)
            , m_frameLtrOff(1)
            , m_frameLtrReassign(0)
            , m_LtrOrder(-1)
            , m_LtrQp(0)
            , m_RefOrder(-1)
            , m_RefQp(0)
            , m_idxScd(0)
#ifdef MFX_ENABLE_EXT
            , m_wsSubSamplingEv(0)
            , m_wsSubSamplingTask(0)
            , m_wsGpuImage(0)
            , m_wsIdxGpuImage(0)
#endif
            , m_Yscd(0)
#ifdef MFX_ENABLE_AVC_CUSTOM_QMATRIX
            , m_qMatrix()
#endif
#if defined(MFX_ENABLE_PARTIAL_BITSTREAM_OUTPUT)
            , m_procBO{0,0}
            , m_scanBO{0,0}
            , m_nextMarkerPtr{nullptr, nullptr}
            , m_curNALtype()
            , m_bsPO{}
#endif

        {
            Zero(m_ctrl);
            Zero(m_internalListCtrl);
            Zero(m_handleRaw);
#if defined(MFX_ENABLE_MCTF_IN_AVC)
            Zero(m_handleMCTF);
#endif
            Zero(m_fid);
#ifdef MFX_ENABLE_FADE_DETECTION
            Zero(m_pwt);
#endif
            Zero(m_brcFrameParams);
            Zero(m_brcFrameCtrl);
            m_FrameName[0] = 0;
#ifndef MFX_AVC_ENCODING_UNIT_DISABLE
            m_collectUnitsInfo = false;
            m_headersCache[0].reserve(10);
            m_headersCache[1].reserve(10);
#endif
        }

        bool operator == (const DdiTask& task)
        {
            if(&task == this) return true;
            return false;
        }

        mfxU8 GetFrameType() const
        {
            return m_type[GetFirstField()];
        }

        // 0 - no skip, 1 - normal, 2 - pavp
        mfxU8 SkipFlag() const
        {
            if (    m_ctrl.SkipFrame == 0
                || (m_type.top & MFX_FRAMETYPE_I)
                || (m_type.bot & MFX_FRAMETYPE_I))
                return 0;
            return (mfxU8)m_ctrl.SkipFrame;
        }

        // Checking timeout for TDR hang detection
        bool CheckForTDRHang(mfxU32 curTime, mfxU32 timeout) const
        {
            MFX_TRACE_D(curTime);
            MFX_TRACE_D(m_startTime);
            MFX_TRACE_D(timeout);

            if (m_startTime && timeout && (curTime - m_startTime) > timeout)
            {
                MFX_TRACE_S("Possible TDR hang:");
                MFX_TRACE_D(((curTime - m_startTime) > timeout));
                return true;
            }

            return false;
        }
        inline void InitBRCParams()
        {
            Zero(m_brcFrameParams);
            Zero(m_brcFrameCtrl);

            m_brcFrameParams.FrameType = m_type[m_fid[0]];
            m_brcFrameParams.DisplayOrder = m_frameOrder;
            m_brcFrameParams.EncodedOrder = m_encOrder;
            m_brcFrameParams.PyramidLayer = (m_brcFrameParams.FrameType & MFX_FRAMETYPE_B) ? (mfxU16)m_loc.level : 0;
            m_brcFrameParams.LongTerm = (m_longTermFrameIdx != NO_INDEX_U8) ? 1 : 0;
            m_brcFrameParams.SceneChange = (mfxU16)m_SceneChange;
            if (!m_brcFrameParams.PyramidLayer && (m_type[m_fid[0]] & MFX_FRAMETYPE_P) && m_LowDelayPyramidLayer)
                m_brcFrameParams.PyramidLayer = (mfxU16)m_LowDelayPyramidLayer;
            m_brcFrameParams.picStruct = GetPicStructForEncode();
#if defined(MFX_ENABLE_ENCTOOLS_LPLA)
            m_brcFrameParams.OptimalFrameSizeInBytes = m_lplastatus.TargetFrameSize;
#endif
#if defined(MFX_ENABLE_ENCTOOLS)
            m_brcFrameParams.LaAvgEncodedSize        = m_lplastatus.AvgEncodedBits;
            m_brcFrameParams.LaCurEncodedSize        = m_lplastatus.CurEncodedBits;
            m_brcFrameParams.LaIDist                 = m_lplastatus.DistToNextI;
#endif
        }
        inline bool isSEIHRDParam(mfxExtCodingOption const & extOpt, mfxExtCodingOption2 const & extOpt2)
        {
            return (((GetFrameType() & MFX_FRAMETYPE_IDR) ||
                ((GetFrameType() & MFX_FRAMETYPE_I) && (extOpt2.BufferingPeriodSEI == MFX_BPSEI_IFRAME))) &&
                (IsOn(extOpt.VuiNalHrdParameters) || IsOn(extOpt.VuiVclHrdParameters)));
        }

        mfxEncodeCtrl   m_ctrl;
        DdiTask *       m_pushed;         // task which was pushed to queue when this task was chosen for encoding
        Pair<mfxU8>     m_type;           // encoding type (one for each field)
        mfxI16          m_QPdelta;
        bool            m_bQPDelta;
        mfxU16          m_QPmodulation;
        mfxU32          m_currGopRefDist;

        // all info about references
        // everything is in pair because task is a per-frame object
        Pair<ArrayDpbFrame>   m_dpb;
        ArrayDpbFrame         m_dpbPostEncoding; // dpb after encoding a frame (or 2 fields)
        Pair<ArrayU8x33>      m_list0;
        Pair<ArrayU8x33>      m_list1;
        Pair<ArrayRefListMod> m_refPicList0Mod;
        Pair<ArrayRefListMod> m_refPicList1Mod;
        Pair<mfxU32>          m_initSizeList0;
        Pair<mfxU32>          m_initSizeList1;

        // currently used for dpb control when svc temporal layers enabled
        mfxExtAVCRefListCtrl  m_internalListCtrl;
        bool                  m_internalListCtrlPresent;
        bool                  m_internalListCtrlHasPriority;
        bool                  m_internalListCtrlRefModLTR;
        bool                  m_keyReference; // frame should be stored in DPB as long as possible

        mfxU32  m_initCpbRemoval;       // initial_cpb_removal_delay
        mfxU32  m_initCpbRemovalOffset; // initial_cpb_removal_delay_offset
        PairU32 m_cpbRemoval;           // one for each field
        mfxU32  m_dpbOutputDelay;       // one for entire frame
        mfxU32  m_encOrder;
        mfxU32  m_encOrderIdr;
        mfxU32  m_encOrderI;

        Pair<DecRefPicMarkingRepeatInfo> m_decRefPicMrkRep; // for sei_message() which repeat dec_ref_pic_marking() of previous frame
        Pair<DecRefPicMarkingInfo>       m_decRefPicMrk;    // dec_ref_pic_marking() for current frame

        mfxU32  m_viewIdx;
        mfxU32  m_idx;                  // index in chain of raw surfaces (set only when sysmem at input)
        PairU32 m_idxBs;                // index of bitstream surfaces, 2 - for interlaced frame (snb only)
        mfxU32  m_idxBsOffset;          // offset for multi-view coding
        mfxU32  m_idxRecon;             // index of reconstructed surface
        mfxU32  m_idxInterLayerRecon;   // index of reconstructed surface for inter-layer prediction
        mfxU32  m_idxReconOffset;       // offset for multi-view coding
        mfxU16  m_idrPicId;
        PairU8  m_subMbPartitionAllowed;
        PairI8  m_cqpValue;
        PairU8  m_insertAud;
        PairU8  m_insertSps;
        PairU8  m_insertPps;
        PairU8  m_AUStartsFromSlice;
        PairU8  m_nalRefIdc;
        PairU32 m_statusReportNumber;
        mfxU32  m_pid;                  // priority_id
        PairU32 m_fillerSize;
// MVC BD {
        PairU32 m_addRepackSize; // for SNB/IVB: size of padding to compensate re-pack of AVC headers to MVC headers
// MVC BD }
        mfxU32  m_maxIFrameSize;
        mfxU32  m_maxPBFrameSize;

        mfxU16  m_numMbPerSlice;
        PairU16 m_numSlice;

        ArrayRoi        m_roi;
        mfxU16          m_numRoi;
        mfxI8           m_NonRectROIDeltaQpList[16] = {};
        mfxU8           m_NumDeltaQpForNonRectROI;
        mfxU16          m_roiMode;
        ArrayRect       m_dirtyRect;
        mfxU16          m_numDirtyRect;
        ArrayMovingRect m_movingRect;
        mfxU16          m_numMovingRect;

        mfxU32  m_did;                  // dependency_id
        mfxU32  m_qid;                  // quality_id
        mfxU32  m_storeRefBasePicFlag;  // for svc key picture

        mfxBitstream *    m_bs;           // output bitstream

#if defined(MFX_ENABLE_ENCODE_QUALITYINFO)
        mfxExtQualityInfoMode* m_qualityInfoMode = nullptr; // quality info mode
        mfxExtQualityInfoOutput* m_qualityInfoOutput = nullptr; // quality info output
#endif

        PairU32           m_bsDataLength; // bitstream size reported by driver (0 - progr/top, 1 - bottom)
        PairU32           m_numLeadingFF; // driver may insert 0xff in the beginning of coded frame
        PairU8            m_qpY;          // QpY reported by driver
        PairU32           m_mad;          // MAD
        BiFrameLocation   m_loc;
        IntraRefreshState m_IRState;
        mfxU32            m_minFrameSize;

#ifdef MFX_ENABLE_ENCODE_STATS
        mfxExtEncodeStatsOutput* m_encodeStats;
        bool                     m_frameLevelQueryEn = false;
        bool                     m_blockLevelQueryEn = false;
#endif
#if defined(MFX_ENABLE_ENCODE_QUALITYINFO)
        bool m_frameLevelQualityEn = false;
#endif
        char   m_FrameName[32];

        bool m_notProtected;             // Driver returns not protected data
        DdiTask const * m_nextLayerTask; // set to 0 if no nextLayerResolutionChange
        mfxU32  m_repack;
        mfxI32  m_fractionalQP; //if m_fractionalQP > 0 set it value in QM matrices
#ifdef MFX_ENABLE_APQ_LQ
        mfxI32  m_ALQOffset;     // MBQP Offset for Lambda QP adjustment
#endif

        mfxMemId        m_midRaw;       // self-allocated input surface (when app gives input frames in system memory)
        mfxMemId        m_midRec;       // reconstruction
        Pair<mfxMemId>  m_midBit;       // output bitstream
        mfxHDLPair      m_handleRaw;    // native handle to raw surface (self-allocated or given by app)
#if defined(MFX_ENABLE_MCTF_IN_AVC)
        mfxMemId        m_midMCTF;
        mfxHDLPair      m_handleMCTF;   // Handle to MCTF denoised surface
        mfxU32          m_idxMCTF;
        CmSurface2D *   m_cmMCTF;
#endif
#ifdef MFX_ENABLE_EXT
        CmSurface2D *   m_cmRaw;        // CM surface made of m_handleRaw
        CmSurface2D *   m_cmRawLa;      // down-sized input surface for Lookahead
        CmBufferUP *    m_cmMb;         // macroblock data, VME kernel output
        void *          m_cmMbSys;      // pointer to associated system memory buffer
        CmBufferUP *    m_cmRefMb;      // macroblock data, VME kernel output for backward ref
        CmBuffer *      m_cmCurbe;      // control structure for ME & HME kernels
        SurfaceIndex *  m_cmRefs;       // VmeSurfaceG75 for ME kernel
        SurfaceIndex *  m_cmRefsLa;     // VmeSurfaceG75 for 2X kernel
        CmEvent *       m_event;
#endif
        VmeData *       m_vmeData;
        DdiTask const * m_fwdRef;
        DdiTask const * m_bwdRef;

        mfxU8   m_adaptiveTUEnabled;
        mfxU8   m_fieldPicFlag;    // true for frames with interlaced content
        bool    m_singleFieldMode; // true for FEI single-field processing mode

        // m_fid is a remapper of field parity to field order and vise versa.
        // i.e. parity = m_fid[fieldId] and fieldId = m_fid[parity] (fieldId == m_fid[m_fid[fieldId]]).
        // It is useful to switch between these two representation, because DdiTask stores all information
        // according to parity, but all of the per-field extension buffers are attached according to field order
        // (the only exception is mfxExtAVCRefLists).
        mfxU8   m_fid[2];               // progressive fid=[0,0]; tff fid=[0,1]; bff fid=[1,0]
        mfxU8   m_fieldCounter;
        mfxU64  m_timeStamp;

        mfxU8   m_minQP;
        mfxU8   m_maxQP;
        std::vector<mfxU8>   m_disableDeblockingIdc[2];
        std::vector<mfxI8>   m_sliceAlphaC0OffsetDiv2[2];
        std::vector<mfxI8>   m_sliceBetaOffsetDiv2[2];

        bool m_resetBRC;

        mfxU32   m_idxMBQP[2];
        mfxMemId m_midMBQP[2];
        bool     m_isMBQP[2];
        bool     m_isUseRawRef;
        bool     m_isSkipped;
        bool     m_toRecode;

        bool     m_isMBControl;
        mfxMemId m_midMBControl;
        mfxU32   m_idxMBControl;
#if defined(MFX_ENABLE_ENCTOOLS)
        mfxLplastatus m_lplastatus;
#endif
#if defined(MFX_ENABLE_AVC_CUSTOM_QMATRIX)
        mfxU32 m_adaptiveCQMHint;
#endif
#ifdef MFX_ENABLE_FADE_DETECTION
        CmSurface2D *         m_cmRawForHist;
        CmBufferUP *          m_cmHist;     // Histogram data, kernel output
        void *                m_cmHistSys;
        mfxExtPredWeightTable m_pwt[2];     // obtained from fade detection
#endif
        bool     m_isENCPAK;

        std::vector<void*> m_userData;
        std::vector<SliceStructInfo> m_SliceInfo;

        mfxU32 m_startTime;
        eMFXHWType m_hwType;  // keep HW type information
#if defined(MFX_ENABLE_H264_REPARTITION_CHECK)
        mfxU8 m_RepartitionCheck; //  DDI level ForceRepartitionCheck
#endif

#ifndef MFX_AVC_ENCODING_UNIT_DISABLE
        bool m_collectUnitsInfo;
        mutable std::vector<mfxEncodedUnitInfo> m_headersCache[2]; //Headers for every field
#endif
        mfxU32 m_TCBRCTargetFrameSize;
        mfxU32 m_SceneChange;
#if defined(MFX_ENABLE_ENCTOOLS)
        mfxU16 m_SpatialComplexity;
        mfxU16 m_PersistenceMapNZ;
        mfxU8  m_PersistenceMap[128];
#endif
#if defined(MFX_ENABLE_MCTF_IN_AVC)
        mfxU32 m_doMCTFIntraFiltering;
#endif
        mfxU32 m_LowDelayPyramidLayer;
        mfxU32 m_frameLtrOff;
        mfxU32 m_frameLtrReassign;
        mfxI32 m_LtrOrder;
        mfxI32 m_LtrQp;
        mfxI32 m_RefOrder;
        mfxI32 m_RefQp;

        mfxU32         m_idxScd;
#ifdef MFX_ENABLE_EXT
        CmEvent       *m_wsSubSamplingEv;
        CmTask        *m_wsSubSamplingTask;
        CmSurface2DUP *m_wsGpuImage;
        SurfaceIndex  *m_wsIdxGpuImage;
#endif
        mfxU8         *m_Yscd;

        BRCFrameParams  m_brcFrameParams;
        mfxBRCFrameCtrl m_brcFrameCtrl;

#ifdef MFX_ENABLE_AVC_CUSTOM_QMATRIX
        unifiedQMatrixH264 m_qMatrix; // buffer for quantization matrix
#endif
#if defined(MFX_ENABLE_PARTIAL_BITSTREAM_OUTPUT)
        mfxU32       m_procBO[2];
        mfxU32       m_scanBO[2];
        mfxU8*       m_nextMarkerPtr[2];
        mfxU8        m_curNALtype;

        mfxFrameData m_bsPO[2];
#endif
    };

    typedef std::list<DdiTask>::iterator DdiTaskIter;
    typedef std::list<DdiTask>::const_iterator DdiTaskCiter;


    template <size_t N>
    class Regression
    {
    public:
        static const mfxU32 MAX_WINDOW = N;

        Regression() {
            Zero(x);
            Zero(y);
        }
        void Reset(mfxU32 size, mfxF64 initX, mfxF64 initY) {
            windowSize = size;
            normX = initX;
            std::fill_n(x, windowSize, initX);
            std::fill_n(y, windowSize, initY);
            sumxx = initX * initX * windowSize;
            sumxy = initX * initY * windowSize;
        }
        void Add(mfxF64 newx, mfxF64 newy) {
            newy = newy / newx * normX;
            newx = normX;
            sumxy += newx * newy - x[0] * y[0];
            sumxx += newx * newx - x[0] * x[0];
            std::copy(x + 1, x + windowSize, x);
            std::copy(y + 1, y + windowSize, y);
            x[windowSize - 1] = newx;
            y[windowSize - 1] = newy;
        }
        mfxF64 GetCoeff() const {
            return sumxy / sumxx;
        }

    //protected:
    public: // temporary for debugging and dumping
        mfxF64 x[N];
        mfxF64 y[N];
        mfxU32 windowSize = 0;
        mfxF64 normX = 0;
        mfxF64 sumxy = 0;
        mfxF64 sumxx = 0;
    };

    class BrcIface
    {
    public:
        virtual ~BrcIface() {};
        virtual mfxStatus Init(MfxVideoParam  & video) = 0;
        virtual mfxStatus Reset(MfxVideoParam  & video) = 0;
        virtual void Close() = 0;
        virtual void PreEnc(const BRCFrameParams& par, std::vector<VmeData *> const & vmeData) = 0;
        virtual void GetQp(const BRCFrameParams& par, mfxBRCFrameCtrl &frameCtrl) = 0;
        virtual void GetQpForRecode(const BRCFrameParams& par, mfxBRCFrameCtrl &frameCtrl) = 0;
        virtual mfxF32 GetFractionalQp(const BRCFrameParams& par) = 0;
        virtual void SetQp(const BRCFrameParams& par, mfxBRCFrameCtrl &frameCtrl) = 0;
        virtual mfxU32 Report(const BRCFrameParams& par, mfxU32 userDataLength, mfxU32 maxFrameSize, mfxBRCFrameCtrl &frameCtrl) = 0;
        virtual mfxU32 GetMinFrameSize() = 0;

    };

    BrcIface * CreateBrc(MfxVideoParam const & video, MFX_ENCODE_CAPS const & hwCaps);

    class Brc : public BrcIface
    {
    public:
        Brc(BrcIface * impl = 0)
        {
            m_impl.reset(impl);
        }
        void SetImpl(BrcIface * impl)
        {
            m_impl.reset(impl);
        }
        ~Brc()
        {
        }
        mfxStatus Init(MfxVideoParam  & video)
        {
            return m_impl->Init(video);
        }
        mfxStatus Reset(MfxVideoParam  & video)
        {
            return m_impl->Reset(video);
        }
        void Close()
        {
            m_impl->Close();
        }
        void PreEnc(const BRCFrameParams& par, std::vector<VmeData *> const & vmeData)
        {
            m_impl->PreEnc(par, vmeData);
        }
        void GetQp(const BRCFrameParams& par, mfxBRCFrameCtrl &frameCtrl)
        {
            m_impl->GetQp(par, frameCtrl);
        }
        void GetQpForRecode(const BRCFrameParams& par, mfxBRCFrameCtrl &frameCtrl)
        {
            m_impl->GetQpForRecode(par, frameCtrl);
        }
        mfxF32 GetFractionalQp(const BRCFrameParams& par)
        {
            return m_impl->GetFractionalQp(par);
        }
        void SetQp(const BRCFrameParams& par, mfxBRCFrameCtrl &frameCtrl)
        {
            m_impl->SetQp(par, frameCtrl);
        }
        mfxU32 Report(const BRCFrameParams& par, mfxU32 userDataLength, mfxU32 maxFrameSize, mfxBRCFrameCtrl &frameCtrl)
        {
            return m_impl->Report(par, userDataLength, maxFrameSize, frameCtrl);
        }
        mfxU32 GetMinFrameSize()
        {
            return m_impl->GetMinFrameSize();
        }

    private:
        std::unique_ptr<BrcIface> m_impl;
    };

    class UmcBrc : public BrcIface
    {
    public:
        ~UmcBrc() { Close(); }

        mfxStatus Init(MfxVideoParam  & video);
        mfxStatus Reset(MfxVideoParam  & ) { return MFX_ERR_NONE; };
        void Close();

        void GetQp(const BRCFrameParams& par, mfxBRCFrameCtrl &frameCtrl);
        void GetQpForRecode(const BRCFrameParams& par, mfxBRCFrameCtrl &frameCtrl);

        mfxF32 GetFractionalQp(const BRCFrameParams& par);

        void SetQp(const BRCFrameParams& par, mfxBRCFrameCtrl &frameCtrl);

        void PreEnc(const BRCFrameParams& par, std::vector<VmeData *> const & vmeData);

        mfxU32 Report(const BRCFrameParams& par, mfxU32 userDataLength, mfxU32 maxFrameSize, mfxBRCFrameCtrl &frameCtrl);

        mfxU32 GetMinFrameSize();

    private:
        UMC::H264BRC m_impl;
        mfxU32 m_lookAhead = 0;
    };

    class Hrd
    {
    public:
        Hrd();

        void Setup(MfxVideoParam const & par);

        void Reset(MfxVideoParam const & par);

        void RemoveAccessUnit(
            mfxU32 size,
            mfxU32 interlace,
            mfxU32 bufferingPeriod);

        mfxU32 GetInitCpbRemovalDelay() const;

        mfxU32 GetInitCpbRemovalDelayOffset() const;
        mfxU32 GetMaxFrameSize(mfxU32 bufferingPeriod) const;

    private:
        mfxU32 m_bitrate;
        mfxU32 m_rcMethod;
        mfxU32 m_hrdIn90k;  // size of hrd buffer in 90kHz units

        double m_tick;      // clock tick
        double m_trn_cur;   // nominal removal time
        double m_taf_prv;   // final arrival time of prev unit

        bool m_bIsHrdRequired;
    };

    struct sLAThresholds
    {
        mfxU32 minFramesForClassicLA; // number of frames is needed for classic LA, if lookAhead < minFramesForClassicLA -> short LA
        mfxU32 minFramesForStat;      // number of frames at the start of stream which must be analyzed with fixed rate
        mfxU32 minCostCalcPeriod;     // minimum number of frames to calulate  cost. costCalcPeriod >= lookAhead,  costCalcPeriod < rateCalcPeriod
        mfxF64 maxRateRatioLocal;     // maximum allowed ratio = realRate/initialRate, real rate is calculated per costCalcPeriod
        mfxF64 minRateRatioLocal;     // minimum allowed ratio = realRate/initialRate, real rate is calculated per costCalcPeriod
        mfxF64 maxAvgRateRatio;       // maximum allowed ratio = avgRate/initialRate, avg rate is calculated per rateCalcPeriod
        mfxF64 minAvgRateRatio;       // minimum allowed ratio = avgRate/initialRate, avg rate is calculated per rateCalcPeriod
    };


    class LookAheadBrc2 : public BrcIface
    {
    public:
        ~LookAheadBrc2() { Close(); }

        mfxStatus Init (MfxVideoParam  & video);
        mfxStatus Reset(MfxVideoParam  & ) { return MFX_ERR_NONE; };

        void Close();

        void GetQp(const BRCFrameParams& par, mfxBRCFrameCtrl &frameCtrl);
        void GetQpForRecode(const BRCFrameParams& par, mfxBRCFrameCtrl &frameCtrl);
        mfxF32 GetFractionalQp(const BRCFrameParams& ) { assert(0); return 26.0f; }


        void PreEnc(const BRCFrameParams& par, std::vector<VmeData *> const & vmeData);

        mfxU32 Report(const BRCFrameParams& par, mfxU32 userDataLength, mfxU32 maxFrameSize, mfxBRCFrameCtrl &frameCtrl);

        mfxU32 GetMinFrameSize() { return 0; }
        void  SetQp(const BRCFrameParams& par, mfxBRCFrameCtrl &frameCtrl);


    public:
        struct LaFrameData
        {
            mfxU32  encOrder;
            mfxI32  poc;
            mfxI32  deltaQp;
            mfxF64  estRate[52];
            mfxF64  estRateTotal[52];
            mfxU32  interCost;
            mfxU32  intraCost;
            mfxU32  propCost;
            mfxU32  bframe;
        };

    protected:
        sLAThresholds m_thresholds;
        mfxU32  m_lookAhead;
        mfxU32  m_lookAheadDep;
        mfxU16  m_LaScaleFactor;
        mfxU32  m_strength;
        mfxU32  m_totNumMb;
        mfxF64  m_initTargetRate;
        mfxF64  m_currRate;
        mfxU32  m_framesBehind;
        mfxF64  m_bitsBehind;
        mfxI32  m_curBaseQp;
        mfxI32  m_curQp;
        mfxU16  m_qpUpdateRange;
        //mfxF32  m_coef;
        mfxF64  m_fr;
        mfxU16  m_AsyncDepth;
        mfxU16  m_first;
        mfxU16  m_skipped;
        mfxU8  m_QPMin[3]; // for I, P and B
        mfxU8  m_QPMax[3]; // for I, P and B
        mfxU32 m_MaxframeSize[3];
        mfxU32 m_maxFrameSizeForRec;
        mfxU32 m_rateCalcPeriod;
        mfxU32 m_costCalcPeriod;

        AVGBitrate* m_AvgBitrate; //sliding window
        std::unique_ptr<Hrd>  m_hrd;

        std::vector<LaFrameData>    m_laData;
        std::vector<LaFrameData>    m_laDataStat;
        Regression<20>              m_rateCoeffHistory[52];

        void ClearStat(mfxU32 frameOrder);
        void SaveStat(mfxU32 frameOrder);
    };

    class LookAheadCrfBrc : public BrcIface
    {
    public:
        ~LookAheadCrfBrc() { Close(); }

        mfxStatus Init(MfxVideoParam  & video);
        mfxStatus Reset(MfxVideoParam  & ) {return MFX_ERR_NONE;};

        void Close() {}

        void GetQp(const BRCFrameParams& par, mfxBRCFrameCtrl &frameCtrl);
        void GetQpForRecode(const BRCFrameParams& par, mfxBRCFrameCtrl &frameCtrl);
        mfxF32 GetFractionalQp(const BRCFrameParams& /*par*/) { assert(0); return 26.0f; }

        void SetQp(const BRCFrameParams& /*par*/, mfxBRCFrameCtrl & /*frameCtrl*/) { assert(0); }

        void PreEnc(const BRCFrameParams& par, std::vector<VmeData *> const & vmeData);

        mfxU32 Report(const BRCFrameParams& par, mfxU32 userDataLength, mfxU32 maxFrameSize, mfxBRCFrameCtrl &frameCtrl);

        mfxU32 GetMinFrameSize() { return 0; }

    protected:
        mfxU32  m_lookAhead;
        mfxI32  m_crfQuality;
        mfxI32  m_curQp;
        mfxU32  m_totNumMb;
        mfxU32  m_intraCost;
        mfxU32  m_interCost;
        mfxU32  m_propCost;
        mfxU8   m_QPMin[3]; // for I, P and B
        mfxU8   m_QPMax[3]; // for I, P and B

    };
#if defined(MFX_ENABLE_EXT_BRC)
    class H264SWBRC : public BrcIface
    {
    public:
        H264SWBRC()
            : m_minSize(0)
            , m_pBRC(nullptr)
            , m_BRCLocal()
        {
        }
        virtual ~H264SWBRC()
        {
            Close();
        }


        mfxStatus   Init(MfxVideoParam &video)
        {
            mfxStatus sts = MFX_ERR_NONE;
            mfxExtBRC * extBRC = GetExtBuffer(video);


            if (extBRC->pthis)
            {
                m_pBRC = extBRC;
            }
            else
            {
                sts = HEVCExtBRC::Create(m_BRCLocal);
                MFX_CHECK(sts == MFX_ERR_NONE, MFX_ERR_NULL_PTR);
                m_pBRC = &m_BRCLocal;
            }
            sts = m_pBRC->Init(m_pBRC->pthis, &video);
            return sts;
        }
        void  Close()
        {
            m_pBRC->Close(m_pBRC->pthis);
            HEVCExtBRC::Destroy(m_BRCLocal);
        }
        mfxStatus  Reset(MfxVideoParam &video )
        {
            return m_pBRC->Reset(m_pBRC->pthis,&video);
        }
        mfxU32 Report(const BRCFrameParams& par, mfxU32 /*userDataLength*/, mfxU32 /*maxFrameSize*/, mfxBRCFrameCtrl &frameCtrl)
        {
            mfxBRCFrameStatus frame_sts = {};
            mfxBRCFrameParam frame_par = *((mfxBRCFrameParam*)&par);

            mfxStatus sts = m_pBRC->Update(m_pBRC->pthis,&frame_par, &frameCtrl, &frame_sts);
            MFX_CHECK(sts == MFX_ERR_NONE, (mfxU32)UMC::BRC_ERROR); // BRC_ERROR

            m_minSize = frame_sts.MinFrameSize;

            switch (frame_sts.BRCStatus)
            {
            case ::MFX_BRC_OK:
                return  UMC::BRC_OK;
            case ::MFX_BRC_BIG_FRAME:
                return UMC::BRC_ERR_BIG_FRAME;
            case ::MFX_BRC_SMALL_FRAME:
                return UMC::BRC_ERR_SMALL_FRAME;
            case ::MFX_BRC_PANIC_BIG_FRAME:
                return UMC::BRC_ERR_BIG_FRAME |  UMC::BRC_NOT_ENOUGH_BUFFER;
            case ::MFX_BRC_PANIC_SMALL_FRAME:
                return UMC::BRC_ERR_SMALL_FRAME| UMC::BRC_NOT_ENOUGH_BUFFER;
            }
            return MFX_BRC_OK;
        }
        void  GetQp(const BRCFrameParams& par, mfxBRCFrameCtrl &frameCtrl)
        {
            mfxBRCFrameParam frame_par = *((mfxBRCFrameParam*)&par);
            m_pBRC->GetFrameCtrl(m_pBRC->pthis,&frame_par, &frameCtrl);
            frameCtrl.QpY = (mfxU8)mfx::clamp(frameCtrl.QpY, 1, 51);
        }
        void GetQpForRecode(const BRCFrameParams& par, mfxBRCFrameCtrl &frameCtrl)
        {
            GetQp(par, frameCtrl);
        }

        void   SetQp(const BRCFrameParams& /*par*/, mfxBRCFrameCtrl &/*frameCtrl*/)
        {
        }
        mfxU32 GetMinFrameSize()
        {
            return m_minSize;
        }
        mfxF32 GetFractionalQp(const BRCFrameParams& /*par*/) { assert(0); return 26.0f; }

        virtual void        PreEnc(const BRCFrameParams& /*par*/, std::vector<VmeData *> const & /* vmeData */) {}

        virtual bool IsVMEBRC()  {return false;}

    private:
        mfxU32      m_minSize;
        mfxExtBRC * m_pBRC;
        mfxExtBRC   m_BRCLocal;

};
#endif


#if defined(MFX_ENABLE_ENCTOOLS)

constexpr mfxU32 ENCTOOLS_QUERY_TIMEOUT = 5000;

inline bool IsNotDefined(mfxU16 param)
{
    return (param == MFX_CODINGOPTION_UNKNOWN);

}
inline bool IsEncToolsOptOn(mfxExtEncToolsConfig &config)
{
    return
        (IsOn(config.AdaptiveI) ||
            IsOn(config.AdaptiveB) ||
            IsOn(config.AdaptiveRefP) ||
            IsOn(config.AdaptiveRefB) ||
            IsOn(config.SceneChange) ||
            IsOn(config.AdaptiveLTR) ||
            IsOn(config.AdaptivePyramidQuantP) ||
            IsOn(config.AdaptivePyramidQuantB) ||
            IsOn(config.AdaptiveQuantMatrices) ||
            IsOn(config.BRCBufferHints) ||
            IsOn(config.BRC));

}
inline void ResetEncToolsPar(mfxExtEncToolsConfig &config, mfxU16 value)
{
    config.AdaptiveI =
        config.AdaptiveB =
        config.AdaptiveRefP =
        config.AdaptiveRefB =
        config.SceneChange =
        config.AdaptiveLTR =
        config.AdaptivePyramidQuantP =
        config.AdaptivePyramidQuantB =
        config.AdaptiveQuantMatrices =
        config.BRCBufferHints =
        config.BRC = value;

}
static mfxStatus InitCtrl(mfxVideoParam const & par, mfxEncToolsCtrl *ctrl, mfxU16 laMode = MFX_VPP_LOOKAHEAD)
{
    MFX_CHECK_NULL_PTR1(ctrl);

    mfxExtCodingOption *CO   = (mfxExtCodingOption *) mfx::GetExtBuffer(par.ExtParam, par.NumExtParam, MFX_EXTBUFF_CODING_OPTION);
    mfxExtCodingOption2 *CO2 = (mfxExtCodingOption2 *)mfx::GetExtBuffer(par.ExtParam, par.NumExtParam, MFX_EXTBUFF_CODING_OPTION2);
    mfxExtCodingOption3 *CO3 = (mfxExtCodingOption3 *)mfx::GetExtBuffer(par.ExtParam, par.NumExtParam, MFX_EXTBUFF_CODING_OPTION3);
    MFX_CHECK_NULL_PTR3(CO, CO2, CO3);
    mfxExtCodingOptionDDI* extDdi = (mfxExtCodingOptionDDI*)mfx::GetExtBuffer(par.ExtParam, par.NumExtParam, MFX_EXTBUFF_DDI);
    MFX_CHECK_NULL_PTR1(extDdi);


    ctrl->CodecId = par.mfx.CodecId;
    ctrl->CodecProfile = par.mfx.CodecProfile;
    ctrl->CodecLevel = par.mfx.CodecLevel;
    ctrl->LowPower = par.mfx.LowPower;
    ctrl->AsyncDepth = par.AsyncDepth;

    ctrl->FrameInfo = par.mfx.FrameInfo;
    ctrl->IOPattern = par.IOPattern;
    ctrl->MaxDelayInFrames = CO2->LookAheadDepth;
    ctrl->NumRefP = std::min(par.mfx.NumRefFrame, extDdi->NumActiveRefP);
    ctrl->MaxGopSize = par.mfx.GopPicSize;
    ctrl->MaxGopRefDist = par.mfx.GopRefDist;
    ctrl->MaxIDRDist = par.mfx.GopPicSize * (par.mfx.IdrInterval + 1);
    ctrl->BRefType = CO2->BRefType;

    ctrl->ScenarioInfo = CO3->ScenarioInfo;
    ctrl->GopOptFlag = (mfxU8)par.mfx.GopOptFlag;

    // Rate control info
    mfxU32 mult = par.mfx.BRCParamMultiplier ? par.mfx.BRCParamMultiplier : 1;
    bool   BRC = (par.mfx.RateControlMethod == MFX_RATECONTROL_CBR ||
        par.mfx.RateControlMethod == MFX_RATECONTROL_VBR);

    ctrl->RateControlMethod = par.mfx.RateControlMethod;  //CBR, VBR, CRF,CQP

    if (!BRC)
    {
        ctrl->QPLevel[0] = par.mfx.QPI;
        ctrl->QPLevel[1] = par.mfx.QPP;
        ctrl->QPLevel[2] = par.mfx.QPB;
    }

    else
    {
        ctrl->TargetKbps = par.mfx.TargetKbps*mult;
        ctrl->MaxKbps = par.mfx.MaxKbps*mult;

        ctrl->HRDConformance = MFX_BRC_NO_HRD;
        if (!IsOff(CO->NalHrdConformance) && !IsOff(CO->VuiNalHrdParameters))
            ctrl->HRDConformance = MFX_BRC_HRD_STRONG;
        else if (IsOn(CO->NalHrdConformance) && IsOff(CO->VuiNalHrdParameters))
            ctrl->HRDConformance = MFX_BRC_HRD_WEAK;


        if (ctrl->HRDConformance)
        {
            ctrl->BufferSizeInKB = par.mfx.BufferSizeInKB*mult;      //if HRDConformance is ON
            ctrl->InitialDelayInKB = par.mfx.InitialDelayInKB*mult;    //if HRDConformance is ON
        }
        else
        {
            ctrl->ConvergencePeriod = 0;     //if HRDConformance is OFF, 0 - the period is whole stream,
            ctrl->Accuracy = 10;              //if HRDConformance is OFF
        }
        ctrl->WinBRCMaxAvgKbps = CO3->WinBRCMaxAvgKbps*mult;
        ctrl->WinBRCSize = CO3->WinBRCSize;
        ctrl->MaxFrameSizeInBytes[0] = CO3->MaxFrameSizeI ? CO3->MaxFrameSizeI : CO2->MaxFrameSize;     // MaxFrameSize limitation
        ctrl->MaxFrameSizeInBytes[1] = CO3->MaxFrameSizeP ? CO3->MaxFrameSizeP : CO2->MaxFrameSize;
        ctrl->MaxFrameSizeInBytes[2] = CO2->MaxFrameSize;

        ctrl->MinQPLevel[0] = CO2->MinQPI;       //QP range  limitations
        ctrl->MinQPLevel[1] = CO2->MinQPP;
        ctrl->MinQPLevel[2] = CO2->MinQPB;

        ctrl->MaxQPLevel[0] = CO2->MaxQPI;       //QP range limitations
        ctrl->MaxQPLevel[1] = CO2->MaxQPP;
        ctrl->MaxQPLevel[2] = CO2->MaxQPB;

        ctrl->PanicMode = CO3->BRCPanicMode;

    }
    if (ctrl->NumExtParam > 1)
    {
        ctrl->ExtParam[0] = mfx::GetExtBuffer(par.ExtParam, par.NumExtParam, MFX_EXTBUFF_ENCTOOLS_DEVICE);
        ctrl->ExtParam[1] = mfx::GetExtBuffer(par.ExtParam, par.NumExtParam, MFX_EXTBUFF_ENCTOOLS_ALLOCATOR);
    }

    // LaScale here
    ctrl->LaScale = 0;
    ctrl->LaQp = 30;
    if (ctrl->ScenarioInfo == MFX_SCENARIO_GAME_STREAMING) 
    {
        mfxU16 crW = par.mfx.FrameInfo.CropW ? par.mfx.FrameInfo.CropW : par.mfx.FrameInfo.Width;
        mfxU16 crH = par.mfx.FrameInfo.CropH ? par.mfx.FrameInfo.CropH : par.mfx.FrameInfo.Height;
        if (crW * crH >= 1920 * 1080) ctrl->LaScale = 2;
        else if (crW * crH >= 1280 * 720) ctrl->LaScale = 1;
    }
    else 
    {
        mfxU16 crH = par.mfx.FrameInfo.CropH ? par.mfx.FrameInfo.CropH : par.mfx.FrameInfo.Height;
        mfxU16 crW = par.mfx.FrameInfo.CropW ? par.mfx.FrameInfo.CropW : par.mfx.FrameInfo.Width;
        mfxU16 maxDim = std::max(crH, crW);
        mfxU16 minDim = std::min(crH, crW);
        constexpr mfxU16 LaScale = 2;
        if (maxDim >= 720 && 
            minDim >= (128<<LaScale)) //encoder limitation, 128 and up is fine
        {
            ctrl->LaScale = LaScale;
        }
        ctrl->LaQp = 26;
    }
    ctrl->LAMode = laMode;
    return MFX_ERR_NONE;
}
class H264EncTools
{
public:

    virtual ~H264EncTools()
    {
       Close();
    }


    static bool isEncToolNeeded(MfxVideoParam &video)
    {
        mfxEncTools * encTools = GetExtBuffer(video);
        mfxExtEncToolsConfig *config = GetExtBuffer(video);

        if (!(encTools && encTools->Context) && (!config))
            return false;

        mfxExtEncToolsConfig tmpConfig = {};
        if (config)
            tmpConfig = *config;

        GetRequiredFunc(video, tmpConfig);

        return IsEncToolsOptOn(tmpConfig);
    }
    bool IsPreEncNeeded()
    {
        if (m_pEncTools )
        {
            return
                (IsOn(m_EncToolConfig.AdaptiveI) ||
                    IsOn(m_EncToolConfig.AdaptiveB) ||
                    IsOn(m_EncToolConfig.AdaptiveRefP) ||
                    IsOn(m_EncToolConfig.AdaptiveRefB) ||
                    IsOn(m_EncToolConfig.SceneChange) ||
                    IsOn(m_EncToolConfig.AdaptiveLTR) ||
                    IsOn(m_EncToolConfig.AdaptivePyramidQuantP) ||
                    IsOn(m_EncToolConfig.AdaptivePyramidQuantB) ||
                    IsOn(m_EncToolConfig.BRCBufferHints) ||
                    IsOn(m_EncToolConfig.AdaptiveMBQP));
        }
        return false;
    }
 
 


    inline bool IsAdaptiveQuantMatrices()
    {
        return (m_pEncTools != 0 &&
            (IsOn(m_EncToolConfig.AdaptiveQuantMatrices)));
    }
    inline bool IsAdaptiveI()
    {
        return (m_pEncTools != 0 &&
            (IsOn(m_EncToolConfig.AdaptiveI)));
    }
    inline bool IsAdaptiveGOP()
    {
        return (m_pEncTools != 0 &&
            (IsOn(m_EncToolConfig.AdaptiveI) ||
            IsOn(m_EncToolConfig.AdaptiveB)));
    }
    inline bool IsAdaptiveQP()
    {
        return (m_pEncTools != 0 &&
            (IsOn(m_EncToolConfig.AdaptivePyramidQuantP) ||
            IsOn(m_EncToolConfig.AdaptivePyramidQuantB)));
    }
    inline bool IsAdaptiveRef()
    {
        return (m_pEncTools != 0 &&
            (IsOn(m_EncToolConfig.AdaptiveRefP) ||
            IsOn(m_EncToolConfig.AdaptiveRefB)));
    }
    inline bool IsAdaptiveLTR()
    {
        return (m_pEncTools != 0 &&
            IsOn(m_EncToolConfig.AdaptiveLTR));
    }
    inline bool IsBRC()
    {
        return (m_pEncTools != 0 &&
            IsOn(m_EncToolConfig.BRC));
    }

    //GetPreEncDelay returns 0 if any error in EncTools configuration.
    static mfxU32 GetPreEncDelay(const MfxVideoParam &par)
    {
        MfxVideoParam video = par;
        if (!isEncToolNeeded(video))
            return 0;

        bool bCreated = false;

        mfxEncTools *encTools = 0;
        mfxExtEncToolsConfig supportedConfig = {};
        mfxExtEncToolsConfig requiredConfig = {};
        mfxEncToolsCtrl ctrl = {};

        mfxStatus sts = CreateEncTools(video, encTools, bCreated);
        if (MFX_FAILED(sts))
            return 0;

        sts = InitCtrl(video, &ctrl);
        if (MFX_FAILED(sts))
        {
            if (bCreated)
                MFXVideoENCODE_DestroyEncTools(encTools);
            return 0;
        }

        encTools->GetSupportedConfig(encTools->Context, &supportedConfig,&ctrl);

        CorrectVideoParams(video, supportedConfig);
        GetRequiredFunc(video, requiredConfig);

        mfxU32 numFramesForDelay = 0;

        encTools->GetDelayInFrames(encTools->Context, &requiredConfig, &ctrl,&numFramesForDelay);

        if (bCreated)
            MFXVideoENCODE_DestroyEncTools(encTools);

        return numFramesForDelay;
    }

    static mfxStatus Query(MfxVideoParam &video)
    {
        mfxExtEncToolsConfig supportedConfig = {};
        mfxExtEncToolsConfig requiredConfig = {};
        mfxEncToolsCtrl ctrl = {};

        mfxEncTools *encTools = 0;
        bool bCreated = false;

        mfxStatus sts = CreateEncTools(video, encTools, bCreated);
        MFX_CHECK_STS(sts);
        sts = InitCtrl(video, &ctrl);
        if (MFX_FAILED(sts))
        {
            if (bCreated)
                MFXVideoENCODE_DestroyEncTools(encTools);
            return sts;
        }

        encTools->GetSupportedConfig(encTools->Context, &supportedConfig,&ctrl);

        if (CorrectVideoParams(video, supportedConfig))
            sts = MFX_WRN_INCOMPATIBLE_VIDEO_PARAM;

        if (bCreated)
            MFXVideoENCODE_DestroyEncTools(encTools);

        return sts;
    }

    mfxStatus  Init(MfxVideoParam &video, eMFXHWType platform)
    {
        MFX_AUTO_LTRACE(MFX_TRACE_LEVEL_INTERNAL, "class H264EncTools::Init");
        mfxExtEncToolsConfig requiredConf = {};
        mfxExtEncToolsConfig supportedConf = {};

#if defined(MFX_ENABLE_ENCTOOLS)
#endif
        memset(&m_EncToolCtrl, 0, sizeof(mfxEncToolsCtrl));
        m_EncToolCtrl.ExtParam = m_ExtParam;
        m_EncToolCtrl.NumExtParam = 2;

#if defined(MFX_ENABLE_ENCTOOLS)
#endif
        mfxU16 laMode = CommonCaps::IsFastPassLASupported(platform, video.mfx.FrameInfo.ChromaFormat) ? (mfxU16)MFX_FASTPASS_LOOKAHEAD : (mfxU16)MFX_VPP_LOOKAHEAD;
        mfxStatus sts = InitCtrl(video, &m_EncToolCtrl, laMode);
        MFX_CHECK_STS(sts);

        sts = CreateEncTools(video, m_pEncTools, m_bEncToolsCreated);
        MFX_CHECK_STS(sts);

#if defined(MFX_ENABLE_ENCTOOLS)
#endif
        m_pEncTools->GetSupportedConfig(m_pEncTools->Context, &supportedConf, &m_EncToolCtrl);

#if defined(MFX_ENABLE_ENCTOOLS)
#endif
        if (CorrectVideoParams(video, supportedConf))
            MFX_RETURN(MFX_ERR_INCOMPATIBLE_VIDEO_PARAM);

        GetRequiredFunc(video, requiredConf);

#if defined(MFX_ENABLE_ENCTOOLS)
#endif
        sts = m_pEncTools->Init(m_pEncTools->Context, &requiredConf, &m_EncToolCtrl);
        MFX_CHECK_STS(sts);

        sts = m_pEncTools->GetActiveConfig(m_pEncTools->Context, &m_EncToolConfig);
        MFX_CHECK_STS(sts);

        mfxExtEncToolsConfig *pConfig = (mfxExtEncToolsConfig *)mfx::GetExtBuffer(video.ExtParam, video.NumExtParam, MFX_EXTBUFF_ENCTOOLS_CONFIG);
        if (pConfig)
        {
            mfxExtBuffer header = pConfig->Header;
            *pConfig = m_EncToolConfig;
            pConfig->Header = header;
        }
        m_LAHWBRC = IsEnctoolsLAGS(video);
        m_LASWBRC = IsEnctoolsLABRC(video);

#if defined(MFX_ENABLE_ENCTOOLS)
#endif
        return MFX_ERR_NONE;
    }

    void Discard(mfxU32 displayOrder)
    {
        if (m_pEncTools)
            m_pEncTools->Discard(m_pEncTools->Context, displayOrder);
    }

    void  Close()
    {
        if (m_pEncTools)
        {
            m_pEncTools->Close(m_pEncTools->Context);
        }
        if (m_bEncToolsCreated)
            MFXVideoENCODE_DestroyEncTools(m_pEncTools);

        m_bEncToolsCreated = false;
    }
    mfxStatus  Reset(MfxVideoParam &video)
    {
        MFX_CHECK(m_pEncTools != 0, MFX_ERR_NOT_INITIALIZED);

        mfxEncToolsCtrl newCtrl = {};
        mfxExtBuffer* ExtParam;
        mfxExtEncoderResetOption* pRO = (mfxExtEncoderResetOption*)mfx::GetExtBuffer(video.ExtParam, video.NumExtParam, MFX_EXTBUFF_ENCODER_RESET_OPTION);
        if (pRO && pRO->StartNewSequence == MFX_CODINGOPTION_ON)
        {
            ExtParam = &(pRO->Header);
            newCtrl.NumExtParam = 1;
            newCtrl.ExtParam = &ExtParam;
        }
        mfxStatus sts = InitCtrl(video, &newCtrl);
        MFX_CHECK_STS(sts);

        sts = m_pEncTools->Reset(m_pEncTools->Context, &m_EncToolConfig, &newCtrl);
        MFX_CHECK_STS(sts);

        m_EncToolCtrl = newCtrl;

        return MFX_ERR_NONE;
    }

    mfxStatus SubmitForPreEnc(mfxU32 displayOrder, mfxFrameSurface1 *pSurface)
    {
        MFX_CHECK(m_pEncTools != 0, MFX_ERR_NOT_INITIALIZED);
        MFX_CHECK(IsOn(m_EncToolConfig.AdaptiveI) ||
            IsOn(m_EncToolConfig.AdaptiveB) ||
            IsOn(m_EncToolConfig.AdaptivePyramidQuantP) ||
            IsOn(m_EncToolConfig.AdaptivePyramidQuantB) ||
            IsOn(m_EncToolConfig.AdaptiveLTR) ||
            IsOn(m_EncToolConfig.AdaptiveRefB) ||
            IsOn(m_EncToolConfig.AdaptiveRefP) ||
            IsOn(m_EncToolConfig.SceneChange) ||
            IsOn(m_EncToolConfig.AdaptivePyramidQuantP) ||
            IsOn(m_EncToolConfig.AdaptiveMBQP) ||
            IsOn(m_EncToolConfig.BRCBufferHints) ||
            IsOn(m_EncToolConfig.AdaptiveQuantMatrices)
            , MFX_ERR_NOT_INITIALIZED);

        mfxEncToolsTaskParam par = {};
        par.DisplayOrder = displayOrder;
        std::vector<mfxExtBuffer*> extParams;

        mfxEncToolsFrameToAnalyze extFrameData = {};
        if (pSurface)
        {
            extFrameData.Header.BufferId = MFX_EXTBUFF_ENCTOOLS_FRAME_TO_ANALYZE;
            extFrameData.Header.BufferSz = sizeof(extFrameData);
            extFrameData.Surface = pSurface;
            extParams.push_back((mfxExtBuffer *)&extFrameData);
        }

        par.ExtParam = &extParams[0];
        par.NumExtParam = (mfxU16)extParams.size();

        return m_pEncTools->Submit(m_pEncTools->Context, &par);
    }
    

    mfxStatus QueryPreEncRes(mfxU32 displayOrder, mfxEncToolsHintPreEncodeGOP &preEncodeGOP)
    {
        MFX_CHECK(m_pEncTools != 0, MFX_ERR_NOT_INITIALIZED);
        MFX_CHECK(IsOn(m_EncToolConfig.AdaptiveI) ||
            IsOn(m_EncToolConfig.AdaptiveB) ||
            IsOn(m_EncToolConfig.AdaptivePyramidQuantP) ||
            IsOn(m_EncToolConfig.AdaptivePyramidQuantB)
            , MFX_ERR_NOT_INITIALIZED);

        mfxEncToolsTaskParam par = {};
        par.DisplayOrder = displayOrder;
        std::vector<mfxExtBuffer*> extParams;
        preEncodeGOP = {};
        preEncodeGOP.Header.BufferId = MFX_EXTBUFF_ENCTOOLS_HINT_GOP;
        preEncodeGOP.Header.BufferSz = sizeof(preEncodeGOP);

        extParams.push_back((mfxExtBuffer *)&preEncodeGOP);

        par.ExtParam = &extParams[0];
        par.NumExtParam = (mfxU16)extParams.size();

        return m_pEncTools->Query(m_pEncTools->Context, &par, ENCTOOLS_QUERY_TIMEOUT);
    }

    mfxStatus QueryPreEncARef(mfxU32 displayOrder, mfxEncToolsHintPreEncodeARefFrames &preEncodeARef)
    {
        MFX_CHECK(m_pEncTools != 0, MFX_ERR_NOT_INITIALIZED);
        MFX_CHECK(IsOn(m_EncToolConfig.AdaptiveLTR)||
            IsOn(m_EncToolConfig.AdaptiveRefB)||
            IsOn(m_EncToolConfig.AdaptiveRefP), MFX_ERR_NOT_INITIALIZED);

        mfxEncToolsTaskParam par = {};
        par.DisplayOrder = displayOrder;
        std::vector<mfxExtBuffer*> extParams;
        preEncodeARef = {};
        preEncodeARef.Header.BufferId = MFX_EXTBUFF_ENCTOOLS_HINT_AREF;
        preEncodeARef.Header.BufferSz = sizeof(preEncodeARef);

        extParams.push_back((mfxExtBuffer *)&preEncodeARef);

        par.ExtParam = extParams.data();
        par.NumExtParam = (mfxU16)extParams.size();

        return m_pEncTools->Query(m_pEncTools->Context, &par, ENCTOOLS_QUERY_TIMEOUT);
    }

    mfxStatus QueryPreEncSChg(mfxU32 displayOrder, mfxEncToolsHintPreEncodeSceneChange &preEncodeSChg)
    {
        MFX_CHECK(m_pEncTools != 0, MFX_ERR_NOT_INITIALIZED);
        MFX_CHECK(IsOn(m_EncToolConfig.AdaptiveI) ||
            IsOn(m_EncToolConfig.AdaptiveLTR) ||
            IsOn(m_EncToolConfig.BRC), MFX_ERR_NOT_INITIALIZED);

        mfxEncToolsTaskParam par = {};
        par.DisplayOrder = displayOrder;
        std::vector<mfxExtBuffer*> extParams;
        memset(&preEncodeSChg, 0, sizeof(preEncodeSChg));
        preEncodeSChg.Header.BufferId = MFX_EXTBUFF_ENCTOOLS_HINT_SCENE_CHANGE;
        preEncodeSChg.Header.BufferSz = sizeof(preEncodeSChg);

        extParams.push_back((mfxExtBuffer *)&preEncodeSChg);

        par.ExtParam = &extParams[0];
        par.NumExtParam = (mfxU16)extParams.size();

        mfxStatus sts = MFX_ERR_NONE;
        sts = m_pEncTools->Query(m_pEncTools->Context, &par, ENCTOOLS_QUERY_TIMEOUT);
        return sts;
    }

    mfxStatus QueryLookAheadStatus(mfxU32 displayOrder, mfxEncToolsBRCBufferHint *bufHint, mfxEncToolsHintPreEncodeGOP *gopHint, mfxEncToolsHintQuantMatrix *cqmHint, mfxEncToolsHintQPMap* qpMapHint)
    {
        MFX_CHECK(m_pEncTools != 0, MFX_ERR_NOT_INITIALIZED);
        MFX_CHECK(bufHint || gopHint || cqmHint, MFX_ERR_NULL_PTR);

        MFX_CHECK(m_LASWBRC || (m_LAHWBRC &&
            (IsOn(m_EncToolConfig.AdaptiveQuantMatrices) || IsOn(m_EncToolConfig.BRCBufferHints) || IsOn(m_EncToolConfig.AdaptivePyramidQuantP) || 
                IsOn(m_EncToolConfig.AdaptiveMBQP))), MFX_ERR_NOT_INITIALIZED);

        mfxEncToolsTaskParam par = {};
        par.DisplayOrder = displayOrder;
        std::vector<mfxExtBuffer*> extParams;

        if (bufHint)
        {
            *bufHint = {};
            bufHint->Header.BufferId = MFX_EXTBUFF_ENCTOOLS_BRC_BUFFER_HINT;
            bufHint->Header.BufferSz = sizeof(*bufHint);
            bufHint->OutputMode = mfxU16(m_LASWBRC ? MFX_BUFFERHINT_OUTPUT_DISPORDER : MFX_BUFFERHINT_OUTPUT_ENCORDER);
            extParams.push_back((mfxExtBuffer *)bufHint);
        }

        if (gopHint)
        {
            *gopHint = {};
            gopHint->Header.BufferId = MFX_EXTBUFF_ENCTOOLS_HINT_GOP;
            gopHint->Header.BufferSz = sizeof(*gopHint);
            extParams.push_back((mfxExtBuffer *)gopHint);
        }

        if (cqmHint)
        {
            *cqmHint = {};
            cqmHint->Header.BufferId = MFX_EXTBUFF_ENCTOOLS_HINT_MATRIX;
            cqmHint->Header.BufferSz = sizeof(*cqmHint);
            extParams.push_back((mfxExtBuffer *)cqmHint);
        }
        if (qpMapHint && m_LAHWBRC)
        {
            qpMapHint->Header.BufferId = MFX_EXTBUFF_ENCTOOLS_HINT_QPMAP;
            qpMapHint->Header.BufferSz = sizeof(*qpMapHint);
            extParams.push_back((mfxExtBuffer*)qpMapHint);
        }

        par.ExtParam = &extParams[0];
        par.NumExtParam = (mfxU16)extParams.size();

        return m_pEncTools->Query(m_pEncTools->Context, &par, ENCTOOLS_QUERY_TIMEOUT);
    }

    mfxStatus SubmitFrameForEncoding(DdiTask &task)
    {
        MFX_CHECK(m_pEncTools != 0, MFX_ERR_NOT_INITIALIZED);
        mfxEncToolsTaskParam par = {};
        BRCFrameParams *frame_par = &task.m_brcFrameParams;
        par.DisplayOrder = frame_par->DisplayOrder;
        std::vector<mfxExtBuffer*> extParams;
        mfxEncToolsBRCFrameParams extFrameStruct = {};
        mfxEncToolsBRCBufferHint extBRCHints = {};
        mfxEncToolsHintPreEncodeGOP gopHints = {};
        extFrameStruct.Header.BufferId = MFX_EXTBUFF_ENCTOOLS_BRC_FRAME_PARAM ;
        extFrameStruct.Header.BufferSz = sizeof(extFrameStruct);
        extFrameStruct.EncodeOrder = frame_par->EncodedOrder;
        extFrameStruct.FrameType = frame_par->FrameType;
        extFrameStruct.PyramidLayer = frame_par->PyramidLayer;
        extFrameStruct.LongTerm = frame_par->LongTerm;
        extFrameStruct.SceneChange = frame_par->SceneChange;
        extFrameStruct.SpatialComplexity = task.m_SpatialComplexity;
        extFrameStruct.PersistenceMapNZ = task.m_PersistenceMapNZ;
        if (task.m_PersistenceMapNZ)
            memcpy(extFrameStruct.PersistenceMap, task.m_PersistenceMap, sizeof(task.m_PersistenceMap));
        extParams.push_back((mfxExtBuffer *)&extFrameStruct);

        if (frame_par->OptimalFrameSizeInBytes | frame_par->LaAvgEncodedSize)
        {
            extBRCHints.Header.BufferId = MFX_EXTBUFF_ENCTOOLS_BRC_BUFFER_HINT;
            extBRCHints.Header.BufferSz = sizeof(extBRCHints);
            extBRCHints.OptimalFrameSizeInBytes     = frame_par->OptimalFrameSizeInBytes;
            extBRCHints.AvgEncodedSizeInBits        = frame_par->LaAvgEncodedSize;
            extBRCHints.CurEncodedSizeInBits        = frame_par->LaCurEncodedSize;
            extBRCHints.DistToNextI                 = frame_par->LaIDist;
            extParams.push_back((mfxExtBuffer *)&extBRCHints);
        }

        if (task.m_bQPDelta || (task.m_QPmodulation != MFX_QP_MODULATION_NOT_DEFINED))
        {
            gopHints.Header.BufferId = MFX_EXTBUFF_ENCTOOLS_HINT_GOP;
            gopHints.Header.BufferSz = sizeof(gopHints);
            if (task.m_bQPDelta)
                gopHints.QPDelta = task.m_QPdelta;
            if (task.m_QPmodulation != MFX_QP_MODULATION_NOT_DEFINED)
                gopHints.QPModulation = task.m_QPmodulation;
            extParams.push_back((mfxExtBuffer *)&gopHints);
        }

        par.ExtParam = &extParams[0];
        par.NumExtParam = (mfxU16)extParams.size();

        return m_pEncTools->Submit(m_pEncTools->Context, &par);
    }

    mfxStatus GetFrameCtrl(mfxBRCFrameCtrl *frame_ctrl, mfxU32 dispOrder, mfxEncToolsHintQPMap* qpMapHint)
    {
        MFX_CHECK(m_pEncTools != 0, MFX_ERR_NOT_INITIALIZED);
        mfxEncToolsTaskParam par = {};
        par.DisplayOrder = dispOrder;
        std::vector<mfxExtBuffer*> extParams;

        mfxEncToolsBRCQuantControl extFrameQP = {};
        extFrameQP.Header.BufferId = MFX_EXTBUFF_ENCTOOLS_BRC_QUANT_CONTROL;
        extFrameQP.Header.BufferSz = sizeof(extFrameQP);
        extParams.push_back((mfxExtBuffer *)&extFrameQP);

        mfxEncToolsBRCHRDPos extHRDPos = {};
        extHRDPos.Header.BufferId = MFX_EXTBUFF_ENCTOOLS_BRC_HRD_POS;
        extHRDPos.Header.BufferSz = sizeof(extHRDPos);
        extParams.push_back((mfxExtBuffer *)&extHRDPos);


        if (qpMapHint)
        {
            qpMapHint->Header.BufferId = MFX_EXTBUFF_ENCTOOLS_HINT_QPMAP;
            qpMapHint->Header.BufferSz = sizeof(*qpMapHint);
            extParams.push_back(&qpMapHint->Header);
        }

        par.ExtParam = &extParams[0];
        par.NumExtParam = (mfxU16)extParams.size();

        mfxStatus sts;
        sts = m_pEncTools->Query(m_pEncTools->Context, &par, ENCTOOLS_QUERY_TIMEOUT);
        MFX_CHECK_STS(sts);

        frame_ctrl->QpY = extFrameQP.QpY;
        frame_ctrl->MaxFrameSize = extFrameQP.MaxFrameSize;
        std::copy(extFrameQP.DeltaQP, extFrameQP.DeltaQP + 8, frame_ctrl->DeltaQP);
        frame_ctrl->MaxNumRepak = extFrameQP.NumDeltaQP;

        frame_ctrl->InitialCpbRemovalDelay = extHRDPos.InitialCpbRemovalDelay;
        frame_ctrl->InitialCpbRemovalOffset = extHRDPos.InitialCpbRemovalDelayOffset;

        return sts;
    }

    mfxStatus SubmitEncodeResult(mfxBRCFrameParam *frame_par, mfxU32 qpY)
    {
        MFX_CHECK(m_pEncTools != 0, MFX_ERR_NOT_INITIALIZED);
        mfxEncToolsTaskParam par;
        par.DisplayOrder = frame_par->DisplayOrder;

        std::vector<mfxExtBuffer*> extParams;
        mfxEncToolsBRCEncodeResult extEncRes;
        extEncRes.Header.BufferId = MFX_EXTBUFF_ENCTOOLS_BRC_ENCODE_RESULT;
        extEncRes.Header.BufferSz = sizeof(extEncRes);
        extEncRes.CodedFrameSize = frame_par->CodedFrameSize;
        extEncRes.QpY = (mfxU16)qpY;
        extEncRes.NumRecodesDone = frame_par->NumRecode;

        extParams.push_back((mfxExtBuffer *)&extEncRes);

        par.ExtParam = &extParams[0];
        par.NumExtParam = (mfxU16)extParams.size();

        return m_pEncTools->Submit(m_pEncTools->Context, &par);
    }

    mfxStatus GetEncodeStatus(mfxBRCFrameStatus *frame_status, mfxU32 dispOrder)
    {
        MFX_CHECK(m_pEncTools != 0, MFX_ERR_NOT_INITIALIZED);

        mfxEncToolsTaskParam par;
        par.DisplayOrder = dispOrder;

        std::vector<mfxExtBuffer*> extParams;
        mfxEncToolsBRCStatus extSts = {};
        extSts.Header.BufferId = MFX_EXTBUFF_ENCTOOLS_BRC_STATUS;
        extSts.Header.BufferSz = sizeof(extSts);
        extParams.push_back((mfxExtBuffer *)&extSts);
        par.ExtParam = &extParams[0];
        par.NumExtParam = (mfxU16)extParams.size();

        mfxStatus sts = m_pEncTools->Query(m_pEncTools->Context, &par, ENCTOOLS_QUERY_TIMEOUT);

        *frame_status = extSts.FrameStatus;
        return sts;
    }

protected:
    static bool CheckSCConditions(MfxVideoParam &video)
    {
        return ((video.mfx.GopRefDist == 0 ||
            video.mfx.GopRefDist == 1 ||
            video.mfx.GopRefDist == 2 ||
            video.mfx.GopRefDist == 4 ||
            video.mfx.GopRefDist == 8) &&
            (video.mfx.FrameInfo.PicStruct == 0 ||
                video.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) &&
            video.calcParam.numTemporalLayer == 0);

    }

    static bool isAdaptiveLTRAllowed(MfxVideoParam& video)
    {
        mfxExtCodingOption3& extOpt3 = GetExtBufferRef(video);
        mfxExtCodingOptionDDI& extDdi = GetExtBufferRef(video);
        return !(extDdi.NumActiveRefP == 1 || IsOff(extOpt3.ExtBrcAdaptiveLTR) || (video.mfx.GopOptFlag & MFX_GOP_STRICT));
    }

    static bool isAdaptiveRefBAllowed(MfxVideoParam& video)
    {
        mfxExtCodingOptionDDI& extDdi = GetExtBufferRef(video);
        bool aRefAllowed = isAdaptiveLTRAllowed(video);
        return aRefAllowed && video.mfx.GopRefDist > 1 && extDdi.NumActiveRefBL0 != 1;
    }

    static void GetRequiredFunc(MfxVideoParam &video, mfxExtEncToolsConfig &config)
    {
        mfxExtCodingOption2  &extOpt2 = GetExtBufferRef(video);
        mfxExtCodingOption3  &extOpt3 = GetExtBufferRef(video);
        mfxExtEncToolsConfig *pConfig = (mfxExtEncToolsConfig *)mfx::GetExtBuffer(video.ExtParam, video.NumExtParam, MFX_EXTBUFF_ENCTOOLS_CONFIG);
        if (pConfig)
            config = *pConfig;

        if (extOpt3.ScenarioInfo != MFX_SCENARIO_GAME_STREAMING)
        {
            if (CheckSCConditions(video))
            {
                bool bAdaptiveI = !(video.mfx.GopOptFlag & MFX_GOP_STRICT) && !IsOff(extOpt2.AdaptiveI);
                bool bHasB = video.mfx.GopRefDist > 1;
                bool bAdaptiveB = !(video.mfx.GopOptFlag & MFX_GOP_STRICT) && !IsOff(extOpt2.AdaptiveB) && bHasB;

                config.AdaptiveI = (mfxU16)(IsNotDefined(config.AdaptiveI) ?
                    (bAdaptiveI ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF) : config.AdaptiveI);
                config.AdaptiveB = (mfxU16)(IsNotDefined(config.AdaptiveB) ?
                    (bAdaptiveB ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF) : config.AdaptiveB);

                config.AdaptivePyramidQuantP = IsNotDefined(config.AdaptivePyramidQuantP) ?
                    (!bHasB ? (mfxU16)MFX_CODINGOPTION_ON : (mfxU16)MFX_CODINGOPTION_OFF) : config.AdaptivePyramidQuantP;
                config.AdaptivePyramidQuantB = IsNotDefined(config.AdaptivePyramidQuantB) ?
                    (bHasB ? (mfxU16)MFX_CODINGOPTION_ON : (mfxU16)MFX_CODINGOPTION_OFF) : config.AdaptivePyramidQuantB;

                bool bAdaptiveRef = isAdaptiveRefBAllowed(video);
                bool bAdaptiveLTR = isAdaptiveLTRAllowed(video);

                config.AdaptiveRefB = IsNotDefined(config.AdaptiveRefB) ?
                    (bAdaptiveRef ? (mfxU16)MFX_CODINGOPTION_ON : (mfxU16)MFX_CODINGOPTION_OFF) : config.AdaptiveRefB;

                config.AdaptiveRefP = IsNotDefined(config.AdaptiveRefP) ?
                    (bAdaptiveLTR ? (mfxU16)MFX_CODINGOPTION_ON : (mfxU16)MFX_CODINGOPTION_OFF) : config.AdaptiveRefP;


                config.AdaptiveLTR = IsNotDefined(config.AdaptiveLTR) ?
                    (bAdaptiveLTR ? (mfxU16)MFX_CODINGOPTION_ON : (mfxU16)MFX_CODINGOPTION_OFF) : config.AdaptiveLTR;

            }
            config.BRC = IsNotDefined(config.BRC) ?
                (((video.mfx.RateControlMethod == MFX_RATECONTROL_CBR ||
                    video.mfx.RateControlMethod == MFX_RATECONTROL_VBR) && extOpt3.ScenarioInfo != MFX_SCENARIO_REMOTE_GAMING) ?
                    (mfxU16)MFX_CODINGOPTION_ON : (mfxU16)MFX_CODINGOPTION_OFF) : config.BRC;

            bool lplaAssistedBRC = (config.BRC == MFX_CODINGOPTION_ON) && (extOpt2.LookAheadDepth > video.mfx.GopRefDist);
            config.BRCBufferHints = (mfxU16)(IsNotDefined(config.BRCBufferHints) ?
                (lplaAssistedBRC ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF) : config.BRCBufferHints);
            config.AdaptiveMBQP = (mfxU16)(IsNotDefined(config.AdaptiveMBQP) ?
                ((lplaAssistedBRC&& IsOn(extOpt2.MBBRC)) ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF) : config.AdaptiveMBQP);
        }

#ifdef MFX_ENABLE_ENCTOOLS_LPLA
        if (extOpt3.ScenarioInfo == MFX_SCENARIO_GAME_STREAMING)
        {
            bool bLA = (extOpt2.LookAheadDepth > 0 &&
                (video.mfx.RateControlMethod == MFX_RATECONTROL_CBR ||
                video.mfx.RateControlMethod == MFX_RATECONTROL_VBR));

            config.BRCBufferHints = (mfxU16)(IsNotDefined(config.BRCBufferHints) ?
                (bLA ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF) : config.BRCBufferHints);

            config.AdaptivePyramidQuantP = (mfxU16)(IsNotDefined(config.AdaptivePyramidQuantP) ?
                (bLA ?  MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF) : config.AdaptivePyramidQuantP);

            config.AdaptiveQuantMatrices = (mfxU16)(IsNotDefined(config.AdaptiveQuantMatrices) ?
                (bLA ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF) : config.AdaptiveQuantMatrices);

            bool bAdaptiveI = !(video.mfx.GopOptFlag & MFX_GOP_STRICT) && IsOn(extOpt2.AdaptiveI);
            bool bAdaptiveB = !(video.mfx.GopOptFlag & MFX_GOP_STRICT) && IsOn(extOpt2.AdaptiveB);

            config.AdaptiveI = (mfxU16)(IsNotDefined(config.AdaptiveI) ?
                (bLA && bAdaptiveI ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF) : config.AdaptiveI);

            config.AdaptiveB = (mfxU16)(IsNotDefined(config.AdaptiveB) ?
                (bLA && bAdaptiveB ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF) : config.AdaptiveB);

        }
#endif
   }

   static void CheckFlag(mfxU16 &tested, mfxU16 ref, mfxU32 &errCount)
   {
       if (IsOn(tested) && IsOff(ref))
       {
           tested = MFX_CODINGOPTION_OFF;
           errCount++;
       }
   }

   static void CheckFlag(mfxU16 &tested, bool bAllowed, mfxU32 &errCount)
   {
       if (IsOn(tested) && (!bAllowed))
       {
           tested = MFX_CODINGOPTION_OFF;
           errCount++;
       }
   }

   static mfxU32 CorrectVideoParams(MfxVideoParam &video, mfxExtEncToolsConfig& supportedConfig)
   {
       mfxExtCodingOption2  &extOpt2 = GetExtBufferRef(video);
       mfxExtCodingOption3  &extOpt3 = GetExtBufferRef(video);

       mfxExtBRC *extBRC = GetExtBuffer(video);
       mfxU32 numChanges = 0;
       mfxExtEncToolsConfig *pConfig = (mfxExtEncToolsConfig *)mfx::GetExtBuffer(video.ExtParam, video.NumExtParam, MFX_EXTBUFF_ENCTOOLS_CONFIG);

       if (pConfig)
       {
           bool bEncToolsCnd = ((video.mfx.FrameInfo.PicStruct == 0 ||
               video.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) &&
               video.calcParam.numTemporalLayer == 0);
           bool bAdaptiveI = !(video.mfx.GopOptFlag & MFX_GOP_STRICT) && !IsOff(extOpt2.AdaptiveI);
           bool bHasB = video.mfx.GopRefDist > 1;
           bool bAdaptiveB = !(video.mfx.GopOptFlag & MFX_GOP_STRICT) && !IsOff(extOpt2.AdaptiveB) && bHasB;
           bool bAdaptiveRef = isAdaptiveRefBAllowed(video);
           bool bAdaptiveLTR = isAdaptiveLTRAllowed(video);

           CheckFlag(pConfig->AdaptiveI, bEncToolsCnd && bAdaptiveI, numChanges);

           CheckFlag(pConfig->AdaptiveB, bEncToolsCnd && bAdaptiveB, numChanges);
           CheckFlag(pConfig->AdaptivePyramidQuantB, bEncToolsCnd && bHasB, numChanges);
           CheckFlag(pConfig->AdaptivePyramidQuantP, bEncToolsCnd && !bHasB, numChanges);
           CheckFlag(pConfig->AdaptiveRefP, bEncToolsCnd && bAdaptiveLTR, numChanges);
           CheckFlag(pConfig->AdaptiveRefB, bEncToolsCnd && bAdaptiveRef, numChanges);
           CheckFlag(pConfig->AdaptiveLTR,  bEncToolsCnd && bAdaptiveLTR, numChanges);
           CheckFlag(pConfig->SceneChange, bEncToolsCnd, numChanges);
           CheckFlag(pConfig->BRCBufferHints, bEncToolsCnd, numChanges);
           CheckFlag(pConfig->AdaptiveQuantMatrices, bEncToolsCnd, numChanges);
           CheckFlag(pConfig->BRC, bEncToolsCnd, numChanges);

           CheckFlag(pConfig->AdaptiveI, supportedConfig.AdaptiveI, numChanges);
           CheckFlag(pConfig->AdaptiveB, supportedConfig.AdaptiveB, numChanges);
           CheckFlag(pConfig->AdaptivePyramidQuantB, supportedConfig.AdaptivePyramidQuantB, numChanges);
           CheckFlag(pConfig->AdaptivePyramidQuantP, supportedConfig.AdaptivePyramidQuantP, numChanges);
           CheckFlag(pConfig->AdaptiveRefP, supportedConfig.AdaptiveRefP, numChanges);
           CheckFlag(pConfig->AdaptiveRefB, supportedConfig.AdaptiveRefB, numChanges);
           CheckFlag(pConfig->AdaptiveLTR, supportedConfig.AdaptiveLTR, numChanges);
           CheckFlag(pConfig->SceneChange, supportedConfig.SceneChange, numChanges);
           CheckFlag(pConfig->BRCBufferHints, supportedConfig.BRCBufferHints, numChanges);
           CheckFlag(pConfig->AdaptiveQuantMatrices, supportedConfig.AdaptiveQuantMatrices, numChanges);
           CheckFlag(pConfig->BRC, supportedConfig.BRC, numChanges);
           CheckFlag(extOpt2.ExtBRC, pConfig->BRC, numChanges);
       }
       CheckFlag(extOpt2.AdaptiveI, supportedConfig.AdaptiveI, numChanges);
       CheckFlag(extOpt2.AdaptiveB, supportedConfig.AdaptiveB, numChanges);
       CheckFlag(extOpt3.ExtBrcAdaptiveLTR, supportedConfig.AdaptiveLTR, numChanges);
       CheckFlag(extOpt2.ExtBRC, supportedConfig.BRC, numChanges);
       CheckFlag(extOpt2.MBBRC, supportedConfig.AdaptiveMBQP, numChanges);

       //ExtBRC isn't compatible with EncTools
       if (extBRC && (extBRC->pthis || extBRC->Init || extBRC->Close  || extBRC->Update || extBRC->Reset))
       {
           extBRC->pthis = 0;
           extBRC->Init = 0;
           extBRC->Close = 0;
           extBRC->Update = 0;
           extBRC->Reset = 0;
           numChanges++;
       }
       if (IsOn(extOpt2.ExtBRC) && (extOpt3.ScenarioInfo ==  MFX_SCENARIO_GAME_STREAMING))
       {
           extOpt2.ExtBRC = MFX_CODINGOPTION_UNKNOWN;
           numChanges++;
       }

       return numChanges;
   }

   static mfxStatus CreateEncTools(MfxVideoParam &video, mfxEncTools * &encTools, bool &bCreated)
   {
       encTools = GetExtBuffer(video);
       bCreated = false;
       if (!(encTools && encTools->Context))
       {
           encTools = MFXVideoENCODE_CreateEncTools(video);
           MFX_CHECK(encTools != 0, MFX_ERR_INVALID_VIDEO_PARAM);
           bCreated = true;
       }
       return MFX_ERR_NONE;
   }
public:
    inline bool isLASWBRC() { return m_LASWBRC; }
    inline bool isLAHWBRC() { return m_LAHWBRC; }
#if defined(MFX_ENABLE_ENCTOOLS)
#endif

private:
    mfxEncTools*            m_pEncTools = nullptr;
    bool                    m_bEncToolsCreated = false;
    mfxEncToolsCtrl         m_EncToolCtrl = {};
    mfxExtEncToolsConfig    m_EncToolConfig = {};
    mfxExtBuffer*           m_ExtParam[2] = {};
    bool                    m_LASWBRC = false;
    bool                    m_LAHWBRC = false;
#if defined(MFX_ENABLE_ENCTOOLS)
#endif

};
#endif

    struct MBQPAllocFrameInfo :
        mfxFrameAllocRequest
    {
        mfxU32 width;  
        mfxU32 height; 
        mfxU32 pitch; 
        mfxU32 height_aligned;
        mfxU32 block_width; 
        mfxU32 block_height; 
    };

    class DdiTask2ndField
    {
    public:
        DdiTask2ndField()
         : m_1stFieldTask(nullptr)
         , m_2ndFieldTask()
        {}

        DdiTask * m_1stFieldTask;
        DdiTask   m_2ndFieldTask;
    };

    // should be called from one thread
    // yields tasks in cyclic manner
    class CyclicTaskPool
    {
    public:
        void Init(mfxU32 size);

        DdiTask2ndField * GetFreeTask();

    private:
        std::vector<DdiTask2ndField>           m_pool;
        std::vector<DdiTask2ndField>::iterator m_next;
    };

    struct MbData
    {
        mfxU32      intraCost;
        mfxU32      interCost;
        mfxU32      propCost;
        mfxU8       w0;
        mfxU8       w1;
        mfxU16      dist;
        mfxU16      rate;
        mfxU16      lumaCoeffSum[4];
        mfxU8       lumaCoeffCnt[4];
        mfxI16Pair  costCenter0;
        mfxI16Pair  costCenter1;
        struct
        {
            mfxU32  intraMbFlag     : 1;
            mfxU32  skipMbFlag      : 1;
            mfxU32  mbType          : 5;
            mfxU32  reserved0       : 1;
            mfxU32  subMbShape      : 8;
            mfxU32  subMbPredMode   : 8;
            mfxU32  reserved1       : 8;
        };
        mfxI16Pair  mv[2]; // in sig-sag scan
    };

    class CmContext;

    struct VmeData
    {
        VmeData()
            : used(false)
            , poc(mfxU32(-1))
            , pocL0(mfxU32(-1))
            , pocL1(mfxU32(-1))
            , encOrder(0)
            , intraCost(0)
            , interCost(0)
            , propCost(0) { }

        bool                used;
        mfxU32              poc;
        mfxU32              pocL0;
        mfxU32              pocL1;
        mfxU32              encOrder;
        mfxU32              intraCost;
        mfxU32              interCost;
        mfxU32              propCost;
        std::vector<MbData> mb;
    };


    class AsyncRoutineEmulator
    {
    public:
        enum {
            STG_ACCEPT_FRAME,
            STG_START_SCD,
            STG_WAIT_SCD,
            STG_START_MCTF,
            STG_WAIT_MCTF,
            STG_START_LA,
            STG_WAIT_LA,
#ifdef MFX_ENABLE_FADE_DETECTION
            STG_START_HIST,
            STG_WAIT_HIST,
#endif
            STG_START_ENCODE,
            STG_WAIT_ENCODE,
            STG_COUNT
        };

        enum {
            STG_BIT_CALL_EMULATOR = 0,
            STG_BIT_ACCEPT_FRAME  = 1 << STG_ACCEPT_FRAME,
            STG_BIT_START_SCD     = 1 << STG_START_SCD,
            STG_BIT_WAIT_SCD      = 1 << STG_WAIT_SCD,
            STG_BIT_START_MCTF    = 1 << STG_START_MCTF,
            STG_BIT_WAIT_MCTF     = 1 << STG_WAIT_MCTF,
            STG_BIT_START_LA      = 1 << STG_START_LA,
            STG_BIT_WAIT_LA       = 1 << STG_WAIT_LA,
#ifdef MFX_ENABLE_FADE_DETECTION
            STG_BIT_START_HIST    = 1 << STG_START_HIST,
            STG_BIT_WAIT_HIST     = 1 << STG_WAIT_HIST,
#endif
            STG_BIT_START_ENCODE  = 1 << STG_START_ENCODE,
            STG_BIT_WAIT_ENCODE   = 1 << STG_WAIT_ENCODE,
            STG_BIT_RESTART       = 1 << STG_COUNT
        };

        AsyncRoutineEmulator();

        AsyncRoutineEmulator(MfxVideoParam const & video,  mfxU32  adaptGopDelay, eMFXHWType platform);

        void Init(MfxVideoParam const & video, mfxU32  adaptGopDelay, eMFXHWType platform);

        mfxU32 GetTotalGreediness() const;

        mfxU32 GetStageGreediness(mfxU32 i) const;

        mfxU32 Go(bool hasInput);

    protected:
        mfxU32 CheckStageOutput(mfxU32 stage);

    private:
        mfxU32 m_stageGreediness[STG_COUNT];
        mfxU32 m_queueFullness[STG_COUNT + 1];
        mfxU32 m_queueFlush[STG_COUNT + 1];
    };

    struct LAOutObject;


    struct QpHistory
    {
        QpHistory() { Reset(); }
        void Reset() { std::fill_n(history, HIST_SIZE, mfxU8(52)); }
        void Add(mfxU32 qp);
        mfxU8 GetAverageQp() const;
    private:
        static const mfxU32 HIST_SIZE = 16;
        mfxU8 history[HIST_SIZE];
    };

    class ImplementationAvc : public VideoENCODE
    {
    public:
        static mfxStatus Query(
            VideoCORE *     core,
            mfxVideoParam * in,
            mfxVideoParam * out,
            void          * state = 0);

        static mfxStatus QueryIOSurf(
            VideoCORE *            core,
            mfxVideoParam *        par,
            mfxFrameAllocRequest * request);

        ImplementationAvc(VideoCORE * core);

        virtual ~ImplementationAvc() override;

        virtual mfxStatus Init(mfxVideoParam * par) override;

        virtual mfxStatus Close() override { return MFX_ERR_NONE; }

        virtual mfxStatus Reset(mfxVideoParam * par) override;

        virtual mfxStatus GetVideoParam(mfxVideoParam * par) override;

        virtual mfxStatus GetFrameParam(mfxFrameParam * par) override;

        virtual mfxStatus GetEncodeStat(mfxEncodeStat * stat) override;

        virtual mfxTaskThreadingPolicy GetThreadingPolicy() override {
            return mfxTaskThreadingPolicy(MFX_TASK_THREADING_INTRA
#if defined(MFX_ENABLE_PARTIAL_BITSTREAM_OUTPUT)
                | (m_isPOut ? MFX_TASK_POLLING : 0)
#endif
            );
        }

        virtual mfxStatus EncodeFrameCheck(
            mfxEncodeCtrl *,
            mfxFrameSurface1 *,
            mfxBitstream *,
            mfxFrameSurface1 **,
            mfxEncodeInternalParams *) override
        {
            MFX_RETURN(MFX_ERR_UNSUPPORTED);
        }

        virtual mfxStatus EncodeFrameCheck(
            mfxEncodeCtrl *           ctrl,
            mfxFrameSurface1 *        surface,
            mfxBitstream *            bs,
            mfxFrameSurface1 **       reordered_surface,
            mfxEncodeInternalParams * internalParams,
            MFX_ENTRY_POINT *         entryPoints,
            mfxU32 &                  numEntryPoints) override;

        virtual mfxStatus EncodeFrameCheckNormalWay(
            mfxEncodeCtrl *           ctrl,
            mfxFrameSurface1 *        surface,
            mfxBitstream *            bs,
            mfxFrameSurface1 **       reordered_surface,
            mfxEncodeInternalParams * internalParams,
            MFX_ENTRY_POINT *         entryPoints,
            mfxU32 &                  numEntryPoints);

        virtual mfxStatus EncodeFrame(
            mfxEncodeCtrl *,
            mfxEncodeInternalParams *,
            mfxFrameSurface1 *,
            mfxBitstream *) override
        {
            MFX_RETURN(MFX_ERR_UNSUPPORTED);
        }

        virtual mfxStatus CancelFrame(
            mfxEncodeCtrl *,
            mfxEncodeInternalParams *,
            mfxFrameSurface1 *,
            mfxBitstream *) override
        {
            MFX_RETURN(MFX_ERR_UNSUPPORTED);
        }

        MFX_PROPAGATE_GetSurface_VideoENCODE_Definition;

    protected:
#if defined(MFX_ENABLE_MCTF_IN_AVC)
        std::unique_ptr<CMC>
            m_mctfDenoiser;

        // m_hvsDenoiser is the replacement for m_mctfDenoiser
        // due to HW change, and reuse the Init, Query, Submit
        // functions.
        std::unique_ptr<MfxVppHelper>
            m_hvsDenoiser;

        mfxStatus SubmitToMctf(
            DdiTask * pTask
        );
        mfxStatus QueryFromMctf(
            void *pParam
        );

        mfxStatus InitMctf(const mfxVideoParam* const par);
#endif

        mfxStatus InitScd(mfxFrameAllocRequest& request);
#ifdef MFX_ENABLE_ASC
        ns_asc::ASC_Cm  amtScd;
#else
        ns_asc::ASC     amtScd;
#endif
        mfxStatus SCD_Put_Frame_Cm(
            DdiTask & newTask);
        mfxStatus SCD_Put_Frame_Hw(
            DdiTask& newTask);
        mfxStatus SCD_Get_FrameType(
            DdiTask & newTask);
        mfxStatus CalculateFrameCmplx(
            DdiTask const &task,
            mfxU32 &raca128);
        mfxStatus Prd_LTR_Operation(
            DdiTask & task);
        void      AssignFrameTypes(
            DdiTask & newTask);
        mfxStatus BuildPPyr(
            DdiTask & task,
            mfxU32 pyrWidth,
            bool bLastFrameUsing,
            bool bResetPyr);

        void setFrameInfo(DdiTask & task,
            mfxU32    fid);

        void setEncUnitsInfo(DdiTask& task,
            mfxU32    fid);

#if defined(MFX_ENABLE_PARTIAL_BITSTREAM_OUTPUT)
        void addPartialOutputOffset(DdiTask & task, mfxU64 offset, bool last = false);
#endif

        mfxStatus UpdateBitstream(
            DdiTask & task,
            mfxU32    fid); // 0 - top/progressive, 1 - bottom

#ifndef MFX_AVC_ENCODING_UNIT_DISABLE
        void FillEncodingUnitsInfo(
            DdiTask &task,
            mfxU8 *sbegin,
            mfxU8 *send,
            mfxExtEncodedUnitsInfo *encUnitsInfo,
            mfxU32 fid);
#endif

        mfxStatus AsyncRoutine(
            mfxBitstream * bs);

        mfxStatus CheckSliceSize(DdiTask &task, bool &bToRecode);
        mfxStatus CheckBufferSize(DdiTask &task, bool &bToRecode, mfxU32 bsDataLength, mfxBitstream * bs);
        mfxStatus CheckBRCStatus(DdiTask &task, bool &bToRecode, mfxU32 bsDataLength);
#ifdef MFX_ENABLE_ENCTOOLS
        mfxStatus EncToolsGetFrameCtrl(DdiTask& task);
#endif
        mfxStatus FillPreEncParams(DdiTask &task);

#if defined(MFX_ENABLE_PARTIAL_BITSTREAM_OUTPUT)
        mfxStatus NextBitstreamDataLength(
            mfxBitstream * bs);
#endif

        void OnNewFrame();
        void SubmitScd();
        void OnScdQueried();
        void OnScdFinished();

        void SubmitMCTF();
        void OnMctfQueried();
        void OnMctfFinished();

        void OnLookaheadSubmitted(DdiTaskIter task);

        void OnLookaheadQueried();
#ifdef MFX_ENABLE_FADE_DETECTION
        void OnHistogramSubmitted();

        void OnHistogramQueried();
#endif
        void OnEncodingSubmitted(DdiTaskIter task);

        void OnEncodingQueried(DdiTaskIter task);

        void BrcPreEnc(DdiTask const & task);

        static mfxStatus AsyncRoutineHelper(
            void * state,
            void * param,
            mfxU32 threadNumber,
            mfxU32 callNumber);

        static mfxStatus UpdateBitstreamData(
            void * state,
            void * param);
#ifdef MFX_ENABLE_EXT
        void SubmitLookahead(
            DdiTask & task);

        mfxStatus QueryLookahead(
            DdiTask & task);
#endif
        mfxStatus QueryStatus(
            DdiTask & task,
            mfxU32    ffid,
            bool      useEvent = true);

        mfxStatus MiniGopSize(
            mfxEncodeCtrl**           ctrl,
            mfxFrameSurface1**        surface,
            mfxU16* requiredFrameType);

        mfxStatus MiniGopSize1(
            mfxEncodeCtrl**           ctrl,
            mfxFrameSurface1**        surface,
            mfxU16* requiredFrameType);

        mfxStatus ProcessAndCheckNewParameters(
            MfxVideoParam & newPar,
            bool & isBRCReset,
            bool & isIdrRequired,
            mfxVideoParam const * newParIn = 0);
#ifdef MFX_ENABLE_EXT
        void DestroyDanglingCmResources();

        CmDevicePtr         m_cmDevice;
#endif
        VideoCORE *         m_core;
        MfxVideoParam       m_video;
        MfxVideoParam       m_videoInit;  // m_video may change by Reset, m_videoInit doesn't change
        mfxEncodeStat       m_stat;
        bool                m_isD3D9SimWithVideoMem;

        std::list<std::pair<mfxBitstream *, mfxU32> > m_listOfPairsForFieldOutputMode;

        AsyncRoutineEmulator m_emulatorForSyncPart;
        AsyncRoutineEmulator m_emulatorForAsyncPart;

        SliceDivider        m_sliceDivider;

        std::list<DdiTask>  m_free;
        std::list<DdiTask>  m_incoming;
        std::list<DdiTask>  m_ScDetectionStarted;
        std::list<DdiTask>  m_ScDetectionFinished;
        std::list<DdiTask>  m_MctfStarted;
        std::list<DdiTask>  m_MctfFinished;
        std::list<DdiTask>  m_reordering;
        std::list<DdiTask>  m_lookaheadStarted;
        std::list<DdiTask>  m_lookaheadFinished;
        std::list<DdiTask>  m_histRun;
        std::list<DdiTask>  m_histWait;
        std::list<DdiTask>  m_encoding;
        UMC::Mutex          m_listMutex;
        DdiTask             m_lastTask;
        mfxU32              m_stagesToGo;
        mfxU32              m_bDeferredFrame;

        mfxU32      m_fieldCounter;
        mfxStatus   m_1stFieldStatus;
        mfxU32      m_frameOrder;
        mfxU32      m_baseLayerOrder;
        mfxU32      m_frameOrderIdrInDisplayOrder;    // frame order of last IDR frame (in display order)
        mfxU32      m_frameOrderIntraInDisplayOrder;  // frame order of last I frame (in display order)
        mfxU32      m_frameOrderIPInDisplayOrder;  // frame order of last I or P frame (in display order)
        mfxU32      m_frameOrderPyrStart;          // frame order of the first frame of pyramid
        mfxU32      m_miniGopCount;
        mfxU32      m_frameOrderStartTScalStructure; // starting point of temporal scalability structure

        // parameters for Intra refresh
        mfxI64      m_baseLayerOrderStartIntraRefresh; // starting point of Intra refresh cycles (could be negative)
        mfxU16      m_intraStripeWidthInMBs; // width of Intra MB stripe (column or row depending on refresh type)

        mfxU32      m_enabledSwBrc;
        Brc         m_brc;
        Hrd         m_hrd;
#if defined(MFX_ENABLE_ENCTOOLS)
        H264EncTools m_encTools;
        bool         m_enabledEncTools;
#if defined(MFX_ENABLE_ENCTOOLS_LPLA)
        std::list<mfxLplastatus> m_lpLaStatus;
#endif
#endif
        mfxU32      m_maxBsSize;

        std::unique_ptr<DriverEncoder>    m_ddi;

        std::unique_ptr<MfxVppHelper> m_vppHelperScaling;

#if defined(MFX_ENABLE_AVC_CUSTOM_QMATRIX)
        QpHistory m_qpHistory;
#endif
        std::vector<mfxU32>     m_recFrameOrder;

        mfxU32 m_recNonRef[2];
#if defined(MFX_ENABLE_MCTF_IN_AVC)
        MfxFrameAllocResponse   m_mctf;
#endif
        MfxFrameAllocResponse   m_scd;
        MfxFrameAllocResponse   m_raw;
        MfxFrameAllocResponse   m_rawSkip;
        MfxFrameAllocResponse   m_rawLa;
        MfxFrameAllocResponse   m_mb;
        MfxFrameAllocResponse   m_curbe;
        MfxFrameAllocResponse   m_rawSys;
        MfxFrameAllocResponse   m_rec;
        MfxFrameAllocResponse   m_bit;
#ifdef MFX_ENABLE_FADE_DETECTION
        MfxFrameAllocResponse   m_histogram;
#endif
        MFX_ENCODE_CAPS         m_caps;
        mfxStatus               m_failedStatus;
        mfxU32                  m_inputFrameType;
        mfxU32                  m_NumSlices;

        MBQPAllocFrameInfo      m_mbqpInfo;
        MfxFrameAllocResponse   m_mbqp;
        bool                    m_useMBQPSurf;

        MfxFrameAllocResponse   m_mbControl;
        bool                    m_useMbControlSurfs;

        std::vector<mfxU8>  m_tmpBsBuf;
        PreAllocatedVector  m_sei;

        eMFXHWType  m_currentPlatform;
        eMFXVAType  m_currentVaType;
        bool        m_isENCPAK;
        bool        m_resetBRC;

#if defined(MFX_ENABLE_PARTIAL_BITSTREAM_OUTPUT)
        bool         m_isPOut;
        int          m_modePOut;
        mfxU32       m_blockPOut;

        std::mutex   m_offsetMutex;
        std::unordered_map<mfxBitstream*, std::queue<uint64_t>> m_offsetsMap;
#endif

#ifdef MFX_ENABLE_EXT
        std::unique_ptr<CmContext>    m_cmCtx;
#endif
        std::vector<VmeData>        m_vmeDataStorage;
        std::vector<VmeData *>      m_tmpVmeData;

        mfxU32      m_LowDelayPyramidLayer;
        mfxI32      m_LtrQp;
        mfxI32      m_LtrOrder;
        mfxI32      m_RefQp;
        mfxI32      m_RefOrder;
#ifdef MFX_ENABLE_ENCODE_STATS
        bool        m_frameLevelQueryEn = false;
        bool        m_blockLevelQueryEn = false;
#endif
#if defined(MFX_ENABLE_ENCODE_QUALITYINFO)
        bool m_frameLevelQualityEn = false;
#endif
    };


    struct NalUnit
    {
        NalUnit() : begin(0), end(0), type(0), numZero(0)
        {}

        NalUnit(mfxU8 * b, mfxU8 * e, mfxU8 t, mfxU8 z) : begin(b), end(e), type(t), numZero(z)
        {}

        mfxU8 * begin;
        mfxU8 * end;
        mfxU8   type;
        mfxU32  numZero;
    };

    NalUnit GetNalUnit(mfxU8 * begin, mfxU8 * end);

    class NaluIterator
    {
    public:
        NaluIterator()
            : m_begin(0)
            , m_end(0)
        {}

        NaluIterator(mfxU8 * begin, mfxU8 * end)
            : m_nalu(GetNalUnit(begin, end))
            , m_begin(m_nalu.end)
            , m_end(end)
        {
        }

        NaluIterator(NalUnit const & nalu, mfxU8 * end)
            : m_nalu(nalu)
            , m_begin(nalu.end)
            , m_end(end)
        {
        }

        NalUnit & operator *()
        {
            return m_nalu;
        }

        NalUnit * operator ->()
        {
            return &m_nalu;
        }

        NaluIterator & operator++()
        {
            m_nalu = GetNalUnit(m_begin, m_end);
            m_begin = m_nalu.end;
            return *this;
        }

        NaluIterator operator++(int)
        {
            NaluIterator tmp;
            ++*this;
            return tmp;
        }

        bool operator ==(NaluIterator const & right) const
        {
            return m_nalu.begin == right.m_nalu.begin && m_nalu.end == right.m_nalu.end;
        }

        bool operator !=(NaluIterator const & right) const
        {
            return !(*this == right);
        }

    private:
        NalUnit m_nalu;
        mfxU8 * m_begin;
        mfxU8 * m_end;
    };

    struct BitstreamDesc
    {
        BitstreamDesc() : begin(0), end(0)
        {}

        mfxU8 * begin;
        mfxU8 * end;
        NalUnit aud;   // byte range within [begin, end)
        NalUnit sps;   // byte range within [begin, end)
        NalUnit pps;   // byte range within [begin, end)
        NalUnit sei;   // first sei if multi sei nal units present
        NalUnit slice; // first slice if multi-sliced
    };

#ifdef MFX_ENABLE_MVC_VIDEO_ENCODE
    class ImplementationMvc : public VideoENCODE
    {
    public:
        static mfxStatus Query(
            VideoCORE *     core,
            mfxVideoParam * in,
            mfxVideoParam * out);

        static mfxStatus QueryIOSurf(
            VideoCORE *            core,
            mfxVideoParam *        par,
            mfxFrameAllocRequest * request);

        ImplementationMvc(
            VideoCORE * core);

// MVC BD {
        void GetInitialHrdState(
            MvcTask & task,
            mfxU32 viewIdx);

        void PatchTask(
            MvcTask const & mvcTask,\
            DdiTask & curTask,
            mfxU32 fieldId);
// MVC BD }

        virtual mfxStatus Init(mfxVideoParam *par) override;

        virtual mfxStatus Close() override;

        virtual mfxTaskThreadingPolicy GetThreadingPolicy() override;

        virtual mfxStatus Reset(mfxVideoParam *par) override;

        virtual mfxStatus GetVideoParam(mfxVideoParam *par) override;

        virtual mfxStatus GetFrameParam(mfxFrameParam *par) override;

        virtual mfxStatus GetEncodeStat(mfxEncodeStat *stat) override;

        virtual mfxStatus EncodeFrameCheck(
            mfxEncodeCtrl *,
            mfxFrameSurface1 *,
            mfxBitstream *,
            mfxFrameSurface1 **,
            mfxEncodeInternalParams *) override
        {
            MFX_RETURN(MFX_ERR_UNSUPPORTED);
        }

        virtual mfxStatus EncodeFrameCheck(
            mfxEncodeCtrl *           ctrl,
            mfxFrameSurface1 *        surface,
            mfxBitstream *            bs,
            mfxFrameSurface1 **       reordered_surface,
            mfxEncodeInternalParams * pInternalParams,
            MFX_ENTRY_POINT           pEntryPoints[],
            mfxU32 &                  numEntryPoints) override;

        virtual mfxStatus EncodeFrame(
            mfxEncodeCtrl *,
            mfxEncodeInternalParams *,
            mfxFrameSurface1 *,
            mfxBitstream *) override
        {
            MFX_RETURN(MFX_ERR_UNSUPPORTED);
        }

        virtual mfxStatus CancelFrame(
            mfxEncodeCtrl *,
            mfxEncodeInternalParams *,
            mfxFrameSurface1 *,
            mfxBitstream *) override
        {
            MFX_RETURN(MFX_ERR_UNSUPPORTED);
        }

        MFX_PROPAGATE_GetSurface_VideoENCODE_Definition;

    protected:
        ImplementationMvc(ImplementationMvc const &);
        ImplementationMvc & operator =(ImplementationMvc const &);

        static mfxStatus TaskRoutineDoNothing(
            void * state,
            void * param,
            mfxU32 threadNumber,
            mfxU32 callNumber);

        static mfxStatus TaskRoutineSubmit(
            void * state,
            void * param,
            mfxU32 threadNumber,
            mfxU32 callNumber);

// MVC BD {
        static mfxStatus TaskRoutineSubmitOneView(
            void * state,
            void * param,
            mfxU32 threadNumber,
            mfxU32 callNumber);

        static mfxStatus TaskRoutineQueryOneView(
            void * state,
            void * param,
            mfxU32 threadNumber,
            mfxU32 callNumber);
// MVC BD }

        static mfxStatus TaskRoutineQuery(
            void * state,
            void * param,
            mfxU32 threadNumber,
            mfxU32 callNumber);

        mfxStatus UpdateBitstream(
            MvcTask & task,
            mfxU32    fieldId); // 0 - top/progressive, 1 - bottom

// MVC BD {
        mfxStatus UpdateBitstreamBaseView(
            DdiTask & task,
            mfxU32    fieldId); // 0 - top/progressive, 1 - bottom
        mfxStatus UpdateBitstreamDepView(
            DdiTask & task,
            mfxU32    fieldId); // 0 - top/progressive, 1 - bottom
        // w/a for SNB/IVB: calculate size of padding to compensate re-pack  of AVC headers to MVC
        mfxU32 CalcPaddingToCompensateRepack(
            DdiTask & task,
            mfxU32 fieldId);

        mfxStatus CopyRawSurface(DdiTask const & task);

        mfxMemId GetRawSurfaceMemId(DdiTask const & task);

        mfxStatus GetRawSurfaceHandle(DdiTask const & task, mfxHDLPair & hdl);
// MVC BD }

        VideoCORE *         m_core;
        MfxVideoParam       m_video;
        MfxVideoParam       m_videoInit;  // m_video may change by Reset, m_videoInit doesn't change
        TaskManagerMvc      m_taskMan;
        MFX_ENCODE_CAPS     m_ddiCaps;
        bool                m_isD3D9SimWithVideoMem;

        std::vector<Hrd>            m_hrd;
// MVC BD {
        std::unique_ptr<DriverEncoder> m_ddi[2];
        mfxU8                        m_numEncs;
        MfxFrameAllocResponse       m_raw[2];
        MfxFrameAllocResponse       m_bitstream[2];
        MfxFrameAllocResponse       m_recon[2];
        // w/a for SNB/IVB: m_spsSubsetSpsDiff is used to calculate padding size for compensation of re-pack AVC headers to MVC
        mfxU32                      m_spsSubsetSpsDiff;
// MVC BD }
        PreAllocatedVector          m_sei;
        std::vector<mfxU8>          m_sysMemBits;
        std::vector<BitstreamDesc>  m_bitsDesc;
        eMFXHWType m_currentPlatform;
// MVC BD {
        std::vector<mfxU16> m_submittedPicStructs[2];
// MVC BD }

        mfxU32                      m_inputFrameType;
#ifdef MFX_ENABLE_MVC_ADD_REF
        mfxI32                      m_bufferSizeModifier; // required to obey HRD conformance after 'dummy' run in ViewOutput mode
#endif // MFX_ENABLE_MVC_ADD_REF
    };
#endif // #ifdef MFX_ENABLE_MVC_VIDEO_ENCODE

    class InputBitstream
    {
    public:
        InputBitstream(
            mfxU8 const * buf,
            size_t        size,
            bool          hasStartCode = true,
            bool          doEmulationControl = true);

        InputBitstream(
            mfxU8 const * buf,
            mfxU8 const * bufEnd,
            bool          hasStartCode = true,
            bool          doEmulationControl = true);

        mfxU32 NumBitsRead() const;
        mfxU32 NumBitsLeft() const;

        mfxU32 GetBit();
        mfxU32 GetBits(mfxU32 nbits);
        mfxU32 GetUe();
        mfxI32 GetSe();

    private:
        mfxU8 const * m_buf;
        mfxU8 const * m_ptr;
        mfxU8 const * m_bufEnd;
        mfxU32        m_bitOff;
        bool          m_doEmulationControl;
    };

    class OutputBitstream
    {
    public:
        OutputBitstream(mfxU8 * buf, size_t size, bool emulationControl = true);
        OutputBitstream(mfxU8 * buf, mfxU8 * bufEnd, bool emulationControl = true);

        mfxU32 GetNumBits() const;

        void PutBit(mfxU32 bit);
        void PutBits(mfxU32 val, mfxU32 nbits);
        void PutUe(mfxU32 val);
        void PutSe(mfxI32 val);
        void PutRawBytes(mfxU8 const * begin, mfxU8 const * end); // startcode emulation is not controlled
        void PutFillerBytes(mfxU8 filler, mfxU32 nbytes);         // startcode emulation is not controlled
        void PutTrailingBits();

    private:
        mfxU8 * m_buf;
        mfxU8 * m_ptr;
        mfxU8 * m_bufEnd;
        mfxU32  m_bitOff;
        bool    m_emulationControl;
    };

    class CabacPackerSimple : public OutputBitstream
    {
    public:
        CabacPackerSimple(mfxU8 * buf, mfxU8 * bufEnd, bool emulationControl = true);
        void EncodeBin(mfxU8  * ctx, mfxU8 bin);
        void TerminateEncode();
    private:
        void PutBitC(mfxU32 B);
        void RenormE();

        mfxU32 m_codILow;
        mfxU32 m_codIRange;
        mfxU32 m_bitsOutstanding;
        mfxU32 m_BinCountsInNALunits;
        bool   m_firstBitFlag;
    };

    void PutSeiHeader(
        OutputBitstream & bs,
        mfxU32            payloadType,
        mfxU32            payloadSize);

    void PutSeiMessage(
        OutputBitstream &                   bs,
        mfxExtAvcSeiBufferingPeriod const & msg);

    void PutSeiMessage(
        OutputBitstream &              bs,
        mfxExtPictureTimingSEI const & extPt,
        mfxExtAvcSeiPicTiming const &  msg);

    void PutSeiMessage(
        OutputBitstream &                   bs,
        mfxExtAvcSeiDecRefPicMrkRep const & msg);

    void PutSeiMessage(
        OutputBitstream &    bs,
        mfxExtAvcSeiRecPoint const & msg);

    mfxU32 PutScalableInfoSeiMessage(
        OutputBitstream &     obs,
        MfxVideoParam const & par);

// MVC BD {
    // Put MVC scalable nested SEI
    void PutSeiMessage(
        OutputBitstream &                   bs,
        mfxU32 needBufferingPeriod,
        mfxU32 needPicTimingSei,
        mfxU32 fillerSize,
        MfxVideoParam const & video,
        mfxExtAvcSeiBufferingPeriod const & msg_bp,
        mfxExtPictureTimingSEI const & extPt,
        mfxExtAvcSeiPicTiming const &  msg_pt);
// MVC BD }


    mfxU8 const * SkipStartCode(mfxU8 const * begin, mfxU8 const * end);
    mfxU8 *       SkipStartCode(mfxU8 *       begin, mfxU8 *       end);

    ArrayRefListMod CreateRefListMod(
        ArrayDpbFrame const &            dpb,
        std::vector<Reconstruct> const & recons,
        ArrayU8x33                       initList,
        ArrayU8x33 const &               modList,
        mfxU32                           curViewIdx,
        mfxI32                           curPicNum,
        bool                             optimize = true);

    mfxU8 * CheckedMFX_INTERNAL_CPY(
        mfxU8 *       dbegin,
        mfxU8 *       dend,
        mfxU8 const * sbegin,
        mfxU8 const * send);

    mfxU8 * CheckedMemset(
        mfxU8 * dbegin,
        mfxU8 * dend,
        mfxU8   value,
        mfxU32  size);

    void ReadRefPicListModification(InputBitstream & reader);

    void ReadDecRefPicMarking(
        InputBitstream & reader,
        bool             idrPicFlag);

    void WriteRefPicListModification(
        OutputBitstream &       writer,
        ArrayRefListMod const & refListMod);

    void WriteDecRefPicMarking(
        OutputBitstream &            writer,
        DecRefPicMarkingInfo const & marking,
        bool                         idrPicFlag);

    mfxU8 * RePackSlice(
        mfxU8 *               dbegin,
        mfxU8 *               dend,
        mfxU8 *               sbegin,
        mfxU8 *               send,
        MfxVideoParam const & par,
        DdiTask const &       task,
        mfxU32                fieldId);

    enum
    {
        RPLM_ST_PICNUM_SUB  = 0,
        RPLM_ST_PICNUM_ADD  = 1,
        RPLM_LT_PICNUM      = 2,
        RPLM_END            = 3,
        RPLM_INTERVIEW_SUB  = 4,
        RPLM_INTERVIEW_ADD  = 5,
    };

    enum
    {
        MMCO_END            = 0,
        MMCO_ST_TO_UNUSED   = 1,
        MMCO_LT_TO_UNUSED   = 2,
        MMCO_ST_TO_LT       = 3,
        MMCO_SET_MAX_LT_IDX = 4,
        MMCO_ALL_TO_UNUSED  = 5,
        MMCO_CURR_TO_LT     = 6,
    };


    void PrepareSeiMessageBuffer(
        MfxVideoParam const & video,
        DdiTask const &       task,
        mfxU32                fieldId, // 0 - top/progressive, 1 - bottom
        PreAllocatedVector &  sei,
        mfxU16 recovery_frame_cnt = 0);

    void PrepareSeiMessageBufferDepView(
        MfxVideoParam const & video,
#ifdef MFX_ENABLE_AVC_BS
        DdiTask &       task,
#else // MFX_ENABLE_AVC_BS
        DdiTask const &       task,
#endif // MFX_ENABLE_AVC_BS
        mfxU32                fieldId, // 0 - top/progressive, 1 - bottom
        PreAllocatedVector &  sei);

#ifdef MFX_ENABLE_SVC_VIDEO_ENCODE
    class SvcTask : public Surface
    {
    public:
        SvcTask()
            : m_layerNum(0)
        {
        }

        void Allocate(mfxU32 layerNum)
        {
            m_layer.reset(new DdiTask *[layerNum]);
            m_layerNum = layerNum;
        }

        mfxU32 LayerNum() const
        {
            return m_layerNum;
        }

        DdiTask *& operator [](mfxU32 i)
        {
            assert(i < m_layerNum);
            assert(m_layer.cptr());
            return m_layer[i];
        }

        DdiTask * const & operator [](mfxU32 i) const
        {
            assert(i < m_layerNum);
            assert(m_layer.cptr());
            return m_layer[i];
        }

    private:
        ScopedArray<DdiTask *> m_layer;
        mfxU32                 m_layerNum;
    };

    class TaskManagerSvc
    {
    public:
        TaskManagerSvc();
        ~TaskManagerSvc();

        void Init(
            VideoCORE *           core,
            MfxVideoParam const & video);

        void Reset(MfxVideoParam const & video); // w/o re-allocation

        void Close();

        mfxStatus AssignTask(
            mfxEncodeCtrl *    ctrl,
            mfxFrameSurface1 * surface,
            mfxBitstream *     bs,
            SvcTask *&         newTask);

        void CompleteTask(SvcTask & task);

        const mfxEncodeStat & GetEncodeStat() const { return m_stat; }

    private:
        mfxStatus AssignAndConfirmAvcTask(
            mfxEncodeCtrl *    ctrl,
            mfxFrameSurface1 * surface,
            mfxBitstream *     bs,
            mfxU32             layerId);

        TaskManagerSvc(TaskManagerSvc const &);
        TaskManagerSvc & operator =(TaskManagerSvc const &);

        VideoCORE *              m_core;
        MfxVideoParam const *    m_video;
        UMC::Mutex               m_mutex;

        ScopedArray<TaskManager> m_layer;
        mfxU32                   m_layerNum;

        std::vector<std::pair<mfxU32, mfxU32> > m_dqid;

        ScopedArray<SvcTask>     m_task;
        mfxU32                   m_taskNum;

        SvcTask *                m_currentTask;
        mfxU32                   m_currentLayerId;
        mfxEncodeStat            m_stat;
    };

    typedef CompleteTaskOnExit<TaskManagerSvc, SvcTask> CompleteTaskOnExitSvc;

    class ImplementationSvc : public VideoENCODE
    {
    public:
        static mfxStatus Query(
            VideoCORE *     core,
            mfxVideoParam * in,
            mfxVideoParam * out);

        static mfxStatus QueryIOSurf(
            VideoCORE *            core,
            mfxVideoParam *        par,
            mfxFrameAllocRequest * request);

        ImplementationSvc(VideoCORE * core);

        virtual ~ImplementationSvc();

        virtual mfxStatus Init(mfxVideoParam * par);

        virtual mfxStatus Close() { return MFX_ERR_NONE; }

        virtual mfxStatus Reset(mfxVideoParam * par);

        virtual mfxStatus GetVideoParam(mfxVideoParam * par);

        virtual mfxStatus GetFrameParam(mfxFrameParam * par);

        virtual mfxStatus GetEncodeStat(mfxEncodeStat * stat);

        virtual mfxTaskThreadingPolicy GetThreadingPolicy() {
            return MFX_TASK_THREADING_INTRA;
        }

        virtual mfxStatus EncodeFrameCheck(
            mfxEncodeCtrl *,
            mfxFrameSurface1 *,
            mfxBitstream *,
            mfxFrameSurface1 **,
            mfxEncodeInternalParams *)
        {
            MFX_RETURN(MFX_ERR_UNSUPPORTED);
        }

        virtual mfxStatus EncodeFrameCheck(
            mfxEncodeCtrl *           ctrl,
            mfxFrameSurface1 *        surface,
            mfxBitstream *            bs,
            mfxFrameSurface1 **       reordered_surface,
            mfxEncodeInternalParams * internalParams,
            MFX_ENTRY_POINT *         entryPoints,
            mfxU32 &                  numEntryPoints);

        virtual mfxStatus EncodeFrame(
            mfxEncodeCtrl *,
            mfxEncodeInternalParams *,
            mfxFrameSurface1 *,
            mfxBitstream *)
        {
            MFX_RETURN(MFX_ERR_UNSUPPORTED);
        }

        virtual mfxStatus CancelFrame(
            mfxEncodeCtrl *,
            mfxEncodeInternalParams *,
            mfxFrameSurface1 *,
            mfxBitstream *)
        {
            MFX_RETURN(MFX_ERR_UNSUPPORTED);
        }

    protected:

        mfxStatus AssignTask(
            mfxEncodeCtrl *    ctrl,
            mfxFrameSurface1 * surface,
            mfxBitstream *     bs,
            SvcTask **         newTask);

        mfxStatus CopyRawSurface(
            DdiTask const & task);

        mfxHDLPair GetRawSurfaceHandle(
            DdiTask const & task);

        mfxStatus UpdateBitstream(
            SvcTask & task,
            mfxU32    fieldId); // 0 - top/progressive, 1 - bottom

        void PrepareSeiMessageBuffer(
            DdiTask const &      task,
            mfxU32               fieldId, // 0 - top/progressive, 1 - bottom
            PreAllocatedVector & sei);

        static mfxStatus TaskRoutineDoNothing(
            void * state,
            void * param,
            mfxU32 threadNumber,
            mfxU32 callNumber);

        static mfxStatus TaskRoutineSubmit(
            void * state,
            void * param,
            mfxU32 threadNumber,
            mfxU32 callNumber);

        static mfxStatus TaskRoutineQuery(
            void * state,
            void * param,
            mfxU32 threadNumber,
            mfxU32 callNumber);

        mfxStatus UpdateDeviceStatus(mfxStatus sts);

        mfxStatus CheckDevice();

    private:
        VideoCORE *                         m_core;
        MfxVideoParam                       m_video;
        MfxVideoParam                       m_videoInit;    // m_video may change by Reset, m_videoInit doesn't change
        Hrd                                 m_hrd;
        std::unique_ptr<DriverEncoder>        m_ddi;
        GUID                                m_guid;
        TaskManagerSvc                      m_manager;
        mfxU32                              m_maxBsSize;
        MfxFrameAllocResponse               m_raw;
        MfxFrameAllocResponse               m_recon;
        MfxFrameAllocResponse               m_bitstream;
        ENCODE_MBDATA_LAYOUT                m_layout;
        MFX_ENCODE_CAPS                     m_caps;
        bool                                m_deviceFailed;
        mfxU32                              m_inputFrameType;
        PreAllocatedVector                  m_sei;
        std::vector<mfxU8>                  m_tmpBsBuf;
        std::vector<mfxU8>                  m_scalabilityInfo;
    };
#endif // #ifdef MFX_ENABLE_SVC_VIDEO_ENCODE

    bool IsInplacePatchNeeded(
        MfxVideoParam const & par,
        DdiTask const &       task,
        mfxU32                fieldId);

    bool IsSlicePatchNeeded(
        DdiTask const & task,
        mfxU32          fieldId);

    mfxStatus  CopyBitstream(
                VideoCORE           & core,
                MfxVideoParam const & video,
                DdiTask const       & task,
                mfxU32              fieldId,
                mfxU8 *             bsData,
                mfxU32              bsSizeAvail);

    mfxU32 GetMaxSliceSize(
        mfxU8 *               sbegin, // contents of source buffer may be modified
        mfxU8 *               send,
        mfxU32                &num);


    mfxStatus UpdateSliceInfo(
        mfxU8 *               sbegin, // contents of source buffer may be modified
        mfxU8 *               send,
        mfxU32                maxSliceSize,
        DdiTask&              task,
        bool&                 bRecoding);

    mfxU8 * PatchBitstream(
        MfxVideoParam const & video,
        DdiTask const &       task,
        mfxU32                fieldId,
        mfxU8 *               sbegin, // contents of source buffer may be modified
        mfxU8 *               send,
        mfxU8 *               dbegin,
        mfxU8 *               dend);
    mfxU8 * InsertSVCNAL(
        DdiTask const &       task,
        mfxU32                fieldId,
        mfxU8 *               sbegin, // contents of source buffer may be modified
        mfxU8 *               send,
        mfxU8 *               dbegin,
        mfxU8 *               dend);

    mfxU8 * AddEmulationPreventionAndCopy(
        mfxU8 *               sbegin,
        mfxU8 *               send,
        mfxU8 *               dbegin,
        mfxU8 *               dend);

    mfxStatus FillSliceInfo(
        DdiTask &           task,
        mfxU32              MaxSliceSize,
        mfxU32              FrameSize,
        mfxU32              widthLa,
        mfxU32              heightLa);

    mfxStatus CorrectSliceInfo(
        DdiTask &           task,
        mfxU32              sliceWeight,
        mfxU32              widthLa,
        mfxU32              heightLa);

    mfxStatus CorrectSliceInfoForsed(
        DdiTask &           task,
        mfxU32              widthLa,
        mfxU32              heightLa);



    mfxU32 CalcBiWeight(
        DdiTask const & task,
        mfxU32 indexL0,
        mfxU32 indexL1);


    mfxI32 GetPicNum(
        ArrayDpbFrame const & dpb,
        mfxU8                 ref);

    mfxI32 GetPicNumF(
        ArrayDpbFrame const & dpb,
        mfxU8                 ref);

    mfxU8 GetLongTermPicNum(
        ArrayDpbFrame const & dpb,
        mfxU8                 ref);

    mfxU32 GetLongTermPicNumF(
        ArrayDpbFrame const & dpb,
        mfxU8                 ref);

    mfxI32 GetPoc(
        ArrayDpbFrame const & dpb,
        mfxU8                 ref);

    DdiTaskIter ReorderFrame(
        ArrayDpbFrame const & dpb,
        DdiTaskIter           begin,
        DdiTaskIter           end);

    DdiTaskIter ReorderFrame(
        ArrayDpbFrame const & dpb,
        DdiTaskIter           begin,
        DdiTaskIter           end,
        bool                  gopStrict,
        bool                  flush,
        bool                  closeGopForSceneChange);

    DdiTaskIter FindFrameToStartEncode(
        MfxVideoParam const & video,
        DdiTaskIter           begin,
        DdiTaskIter           end);

    DdiTaskIter FindFrameToWaitEncode(
        DdiTaskIter begin,
        DdiTaskIter end);
    DdiTaskIter FindFrameToWaitEncodeNext(
        DdiTaskIter begin,
        DdiTaskIter end,
        DdiTaskIter cur);

    PairU8 GetFrameType(
        MfxVideoParam const & video,
        mfxU32                frameOrder);

    IntraRefreshState GetIntraRefreshState(
        MfxVideoParam const & video,
        mfxU32                frameOrderInGop,
        mfxEncodeCtrl const * ctrl,
        mfxU16                intraStripeWidthInMBs,
        SliceDivider &        divider,
        MFX_ENCODE_CAPS       caps);

    mfxStatus UpdateIntraRefreshWithoutIDR(
        MfxVideoParam const & oldPar,
        MfxVideoParam const & newPar,
        mfxU32                frameOrder,
        mfxI64                oldStartFrame,
        mfxI64 &              updatedStartFrame,
        mfxU16 &              updatedStripeWidthInMBs,
        SliceDivider &        divider,
        MFX_ENCODE_CAPS       caps);

    BiFrameLocation GetBiFrameLocation(
        MfxVideoParam const & video,
        mfxU32                frameOrder,
        mfxU32                currGopRefDist,
        mfxU32                miniGOPCount);

    void UpdateDpbFrames(
        DdiTask & task,
        mfxU32    field,
        mfxU32    frameNumMax);

    void InitRefPicList(
        DdiTask & task,
        mfxU32    field);

    void ModifyRefPicLists(
        MfxVideoParam const & video,
        DdiTask &             task,
        mfxU32                fieldId);

    void MarkDecodedRefPictures(
        MfxVideoParam const & video,
        DdiTask &             task,
        mfxU32                fid);

    ArrayRefListMod CreateRefListMod(
        ArrayDpbFrame const & dpb,
        ArrayU8x33            initList,
        ArrayU8x33 const &    modList,
        mfxU32                curViewIdx,
        mfxI32                curPicNum,
        bool                  optimize);

    void ConfigureTask(
        DdiTask &                 task,
        DdiTask const &           prevTask,
        MfxVideoParam const &     video,
        MFX_ENCODE_CAPS const &   caps);

    mfxStatus GetNativeHandleToRawSurface(
        VideoCORE &           core,
        MfxVideoParam const & video,
        DdiTask const &       task,
        mfxHDLPair &          handle,
        bool                  isD3D9SimWithVideoMem);

    bool IsFrameToSkip(DdiTask&  task, MfxFrameAllocResponse & poolRec, std::vector<mfxU32> fo, bool bSWBRC);
    mfxStatus CodeAsSkipFrame(  VideoCORE&            core,
                                MfxVideoParam const & video,
                                DdiTask&       task,
                                MfxFrameAllocResponse & pool,
                                MfxFrameAllocResponse & poolRec);
    mfxStatus CopyRawSurfaceToVideoMemory(
        VideoCORE &           core,
        MfxVideoParam const & video,
        DdiTask const &       task,
        bool                  isD3D9SimWithVideoMem);

    mfxHDL GetNativeHandle(
        VideoCORE & core,
        mfxFrameSurface1& surf,
        bool        external = false);

    void AnalyzeVmeData(
        DdiTaskIter begin,
        DdiTaskIter end,
        mfxU32      width,
        mfxU32      height);

    void CalcPredWeightTable(
        DdiTask & task,
        mfxU32 MaxNum_WeightedPredL0,
        mfxU32 MaxNum_WeightedPredL1);


    struct FindByFrameOrder
    {
        FindByFrameOrder(mfxU32 frameOrder) : m_frameOrder(frameOrder) {}

        template <class T> bool operator ()(T const & task) const
        {
            return task.m_frameOrder == m_frameOrder;
        }

        mfxU32 m_frameOrder;
    };

    bool OrderByFrameNumWrap(DpbFrame const & lhs, DpbFrame const & rhs);
    bool OrderByFrameNumWrapKeyRef(DpbFrame const & lhs, DpbFrame const & rhs);

    template <class T, class P> typename T::pointer find_if_ptr(T & container, P pred)
    {
        typename T::iterator i = std::find_if(container.begin(), container.end(), pred);
        return (i == container.end()) ? 0 : &*i;
    }

    template <class T, class P> typename T::pointer find_if_ptr2(T & container1, T & container2, P pred)
    {
        typename T::pointer p = find_if_ptr(container1, pred);
        if (p == 0)
            p = find_if_ptr(container2, pred);
        return p;
    }
    template <class T, class P> typename T::pointer find_if_ptr3(T & container1, T & container2, T & container3, P pred)
    {
        typename T::pointer p = find_if_ptr(container1, pred);
        if (p == 0)
            p = find_if_ptr(container2, pred);
        if (p == 0)
            p = find_if_ptr(container3, pred);

        return p;
    }
    template <class T, class P> typename T::pointer find_if_ptr4(T & container1, T & container2, T & container3, T & container4, P pred)
    {
        typename T::pointer p = find_if_ptr(container1, pred);
        if (p == 0)
            p = find_if_ptr(container2, pred);
        if (p == 0)
            p = find_if_ptr(container3, pred);
        if (p == 0)
            p = find_if_ptr(container4, pred);

        return p;
    }

#ifdef MFX_ENABLE_AVC_CUSTOM_QMATRIX
    /// <summary>Function reads array[inSize] to array[outSize][outSize] in ZigZag order.</summary>
    /// <description>Function reads array[inSize] to array[outSize][outSize] in ZigZag order.
    /// 1 2 3    1 2 6
    /// 4 5 6 -> 3 5 7
    /// 7 8 9    4 8 9
    /// </description>
    /// <param name="inPtr">Pointer for the input array, has to be non-NULL</param>
    /// <param name="inSize">Size of input array, works only with quad arrays. For example, inSize for matrix 4x4 is 16</param>
    /// <param name="outPtr">Pointer for the output array, has to be non-NULL</param>
    /// <param name="outSize">Size of output buffer, has to be not less than multiplied dimensions of input matrix. For example, outSize for matrix 4x4 has to be greater than or equal to 4x4</param>
    template<class T> void MakeZigZag(void const * const inPtr, const size_t inSize, void * const outPtr, const size_t outSize)
    {
        T const * const in = static_cast<T const * const>(inPtr);
        T * const out = static_cast<T * const>(outPtr);

        assert(inPtr != NULL);
        assert(outPtr != NULL);
        assert((inSize * inSize) <= outSize);
        (void)outSize;

        uint64_t y = 0, x = 0;
        for (uint64_t i = 0, pos = 0, size = (uint64_t)inSize * (uint64_t)inSize; i < inSize; ++i)
        {
            if (y <= x)
            {
                y = i;
                x = 0;
                for (; x <= i; --y, ++x, ++pos)
                {
                    out[y*inSize + x] = in[(pos / inSize)*inSize + pos % inSize];
                    out[(inSize - y - 1)*inSize + inSize - x - 1] = in[((size - pos - 1) / inSize)*inSize + (size - pos - 1) % inSize];
                }
            }
            else
            {
                x = i;
                y = 0;
                for (; y <= i; --x, ++y, ++pos)
                {
                    out[y*inSize + x] = in[(pos / inSize)*inSize + pos % inSize];
                    out[(inSize - y - 1)*inSize + inSize - x - 1] = in[((size - pos - 1) / inSize)*inSize + (size - pos - 1) % inSize];
                }
            }
        }
    }

    /// <summary>Function reads array[inSize][inSize] in ZigZag order to array[outSize].</summary>
    /// <description>Function reads T array[inSize][inSize] in ZigZag order into T array[outSize].
    /// 1 2 6    1 2 3
    /// 3 5 7 -> 4 5 6
    /// 4 8 9    7 8 9
    /// </description>
    /// <param name="inPtr">Pointer for the input array, has to be non-NULL</param>
    /// <param name="inSize">Size of input array, works only with quad arrays. For example, inSize for matrix 4x4 is 4</param>
    /// <param name="outPtr">Pointer for the output array, has to be non-NULL</param>
    /// <param name="outSize">Size of output buffer, has to be not less than inSize*inSize. For example, outSize for matrix 4x4 has to be greater than or equal to 16</param>
    template<class T> void ZigZagToPlane(void const * const inPtr, const size_t inSize, void * const outPtr, const size_t outSize)
    {
        T const * const in = static_cast<T const * const>(inPtr);
        T * const out = static_cast<T * const>(outPtr);

        assert(inPtr != NULL);
        assert(outPtr != NULL);
        assert((inSize * inSize) <= outSize);
        (void)outSize;

        uint64_t y = 0, x = 0;
        for (uint64_t i = 0, pos = 0, size = (uint64_t)inSize * (uint64_t)inSize; i < inSize; ++i)
        {
            if (y <= x)
            {
                y = i;
                x = 0;
                for (; x <= i; --y, ++x, ++pos)
                {
                    out[(pos / inSize)*inSize + pos % inSize] = in[y*inSize + x];
                    out[((size - pos - 1) / inSize)*inSize + (size - pos - 1) % inSize] = in[(inSize - y - 1)*inSize + inSize - x - 1];
                }
            }
            else
            {
                x = i;
                y = 0;
                for (; y <= i; --x, ++y, ++pos)
                {
                    out[(pos / inSize)*inSize + pos % inSize] = in[y*inSize + x];
                    out[((size - pos - 1) / inSize)*inSize + (size - pos - 1) % inSize] = in[(inSize - y - 1)*inSize + inSize - x - 1];
                }
            }
        }
    }
    ///<summary>Function fills custom quantization matrices into inMatrix, writes in ZigZag orde</summary>
    ///<param name="ScenarioInfo">Fills matrices depends on selected ScenarioInfo (CodingOption3)</param>
    ///<return>MFX_ERR_NONE if matrices were copied, MFX_ERR_NOT_FOUND if requested ScenarioInfo isn't supported</return>
    mfxStatus FillCustomScalingLists(void *inMatrix, mfxU16 ScenarioInfo, mfxU8 maxtrixIndex = 0);

    ///<summary>Function fills default values of scaling list</summary>
    ///<param name="outList">Pointer to the output list</param>
    ///<param name="outListSize">Size of output list, has to be not less than 16 for indexes 0..5, and not less than 64 for 6..11</param>
    ///<param name="index">Index of scaling list according Table 7-2 Rec. ITU-T H.264, has to be in range 0..11</param>
    ///<param name="zigzag">Flag, if set to true, output values will be ordered in zigzag order, default - false</param>
    ///<return>MFX_ERR_NONE if default values were copied, MFX_ERR_NOT_FOUND if index not in range 0..11</return>
    mfxStatus GetDefaultScalingList(mfxU8 *outList, mfxU8 outListSize, mfxU8 index, bool zigzag);

    ///<summary>Function fills qMatrix for task based on values provided in extSps and extPps</summary>
    ///<description>Function expects value of extSps.seqScalingMatrixPresentFlag and value of extPps.picScalingMatrixPresentFlag
    ///were verified before call. Otherwise default scaling lists will be used, but it won't be expected behavior</description>
    ///<param name="extSps">SPS header with defined (or not defined) scaling lists</param>
    ///<param name="extPps">PPS header with defined (or not defined) scaling lists</param>
    ///<param name="task">DDI task for the filling scaling lists for current frame</param>
    void FillTaskScalingList(mfxExtSpsHeader const &extSps, mfxExtPpsHeader const &extPps, DdiTask &task);
#endif
}; // namespace MfxHwH264Encode

#endif // _MFX_H264_ENCODE_HW_UTILS_H_
#endif // MFX_ENABLE_H264_VIDEO_ENCODE
