#!/usr/bin/env python3
from pathlib import Path
import sys

MARKER = "twiNX H264 reference/frame guard"


def find_function_end(text, start):
    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError("function body start not found")
    depth = 0
    i = brace
    in_string = False
    in_char = False
    in_line_comment = False
    in_block_comment = False
    escape = False
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if in_line_comment:
            if c == "\n":
                in_line_comment = False
        elif in_block_comment:
            if c == "*" and n == "/":
                in_block_comment = False
                i += 1
        elif in_string:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == '"':
                in_string = False
        elif in_char:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == "'":
                in_char = False
        else:
            if c == "/" and n == "/":
                in_line_comment = True
                i += 1
            elif c == "/" and n == "*":
                in_block_comment = True
                i += 1
            elif c == '"':
                in_string = True
            elif c == "'":
                in_char = True
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return i + 1
        i += 1
    raise RuntimeError("function body end not found")


def replace_function(text, signature, replacement):
    start = text.find(signature)
    if start < 0:
        raise RuntimeError(f"function signature not found: {signature}")
    end = find_function_end(text, start)
    return text[:start] + replacement.rstrip() + text[end:]


HELPERS = r'''
/* twiNX H264 reference/frame guard:
 * Twitch commercial boundaries can leave the H.264 reference lists with
 * incomplete or stale hardware frames. Validate every picture before it
 * reaches the NVDEC command buffer; dropping one damaged frame is safer than
 * submitting a bad reference and corrupting the hardware-frame copy path.
 */
static bool nvtegra_h264_frame_map_usable(const AVFrame *frame) {
    const AVNVTegraFrame *nv_frame;

    if (!frame || !frame->buf[0] || !frame->buf[0]->data ||
        !frame->data[0] || !frame->data[1])
        return false;

    nv_frame = (const AVNVTegraFrame *)frame->buf[0]->data;
    return nv_frame->map_ref && nv_frame->map_ref->data;
}

static bool nvtegra_h264_picture_usable(const H264Picture *pic) {
    if (!pic || !pic->f || !pic->hwaccel_picture_private)
        return false;

    return nvtegra_h264_frame_map_usable(pic->f);
}

static int nvtegra_h264_find_slot(AVCodecContext *avctx, uint32_t *mask,
                                  const char *kind) {
    uint32_t available = (~*mask) & 0xffffu;
    int slot;

    if (!available) {
        av_log(avctx, AV_LOG_ERROR,
               "nvtegra/h264: dropping frame: no free %s slot for reference state\n",
               kind);
        return AVERROR_INVALIDDATA;
    }

    slot = ff_ctz(available);
    *mask |= 1u << slot;
    return slot;
}

static int nvtegra_h264_add_ref(AVCodecContext *avctx, H264Picture **refs,
                                int *num_refs, int max_refs, H264Picture *pic,
                                const char *kind, int index) {
    if (!pic)
        return 0;

    if (*num_refs >= max_refs) {
        av_log(avctx, AV_LOG_WARNING,
               "nvtegra/h264: dropping extra %s reference %d: list is full\n",
               kind, index);
        return 0;
    }

    if (!nvtegra_h264_picture_usable(pic)) {
        av_log(avctx, AV_LOG_WARNING,
               "nvtegra/h264: dropping unusable %s reference %d "
               "(pic=%p frame=%p priv=%p)\n",
               kind, index, pic, pic ? pic->f : NULL,
               pic ? pic->hwaccel_picture_private : NULL);
        return 0;
    }

    refs[(*num_refs)++] = pic;
    return 0;
}
'''

DPB_ADD = r'''
static int dpb_add(AVCodecContext *avctx, nvdec_dpb_entry_s *dst, H264Picture *src) {
    NVTegraH264FrameData *fr_priv;
    int marking;

    if (!nvtegra_h264_picture_usable(src)) {
        av_log(avctx, AV_LOG_ERROR,
               "nvtegra/h264: dropping frame: invalid DPB reference "
               "(pic=%p frame=%p priv=%p)\n",
               src, src ? src->f : NULL,
               src ? src->hwaccel_picture_private : NULL);
        return AVERROR_INVALIDDATA;
    }

    fr_priv = src->hwaccel_picture_private;
    if (!fr_priv->pic_initialized || fr_priv->pic_idx >= 16 ||
        !fr_priv->dpb_initialized || fr_priv->dpb_idx >= 16) {
        av_log(avctx, AV_LOG_ERROR,
               "nvtegra/h264: dropping frame: invalid DPB indices "
               "(pic_idx=%u dpb_idx=%u pic_init=%d dpb_init=%d)\n",
               fr_priv->pic_idx, fr_priv->dpb_idx,
               fr_priv->pic_initialized, fr_priv->dpb_initialized);
        return AVERROR_INVALIDDATA;
    }

    marking = src->long_ref ? 2 : 1;
    *dst = (nvdec_dpb_entry_s){
        .index                = fr_priv->pic_idx,
        .col_idx              = fr_priv->pic_idx,
        .state                = src->reference,
        .is_long_term         = src->long_ref,
        .not_existing         = src->invalid_gap,
        .is_field             = src->field_picture,
        .top_field_marking    = (src->reference & PICT_TOP_FIELD)    ? marking : 0,
        .bottom_field_marking = (src->reference & PICT_BOTTOM_FIELD) ? marking : 0,
        .output_memory_layout = 0, /* NV12 */
        .FieldOrderCnt        = {
            field_poc(src->field_poc, true),
            field_poc(src->field_poc, false),
        },
        .FrameIdx             = src->long_ref ? src->pic_id : src->frame_num,
    };

    return 0;
}
'''

REGISTER_REF = r'''
static inline int register_ref(AVCodecContext *avctx, NVTegraH264DecodeContext *ctx,
                               H264Picture *fr) {
    NVTegraH264FrameData *fr_priv;

    if (!nvtegra_h264_picture_usable(fr)) {
        av_log(avctx, AV_LOG_WARNING,
               "nvtegra/h264: refusing unusable registered reference "
               "(pic=%p frame=%p priv=%p)\n",
               fr, fr ? fr->f : NULL,
               fr ? fr->hwaccel_picture_private : NULL);
        return AVERROR_INVALIDDATA;
    }

    fr_priv = fr->hwaccel_picture_private;
    if (fr_priv->dpb_idx >= 16 || fr_priv->pic_idx >= 16) {
        av_log(avctx, AV_LOG_ERROR,
               "nvtegra/h264: dropping frame: registered reference index "
               "out of range (pic_idx=%u dpb_idx=%u)\n",
               fr_priv->pic_idx, fr_priv->dpb_idx);
        return AVERROR_INVALIDDATA;
    }

    ctx->dpb[fr_priv->dpb_idx] = fr;
    ctx->dpb_mask     |= 1u << fr_priv->dpb_idx;
    ctx->pic_idx_mask |= 1u << fr_priv->pic_idx;
    return 0;
}
'''

FIND_SLOT = r'''
static inline int find_slot(uint32_t *mask) {
    uint32_t available = (~*mask) & 0xffffu;
    int slot;

    if (!available)
        return -1;

    slot = ff_ctz(available);
    *mask |= 1u << slot;
    return slot;
}
'''

PREPARE_FRAME_SETUP = r'''
static int nvtegra_h264_prepare_frame_setup(AVCodecContext *avctx,
                                            nvdec_h264_pic_s *setup,
                                            H264Context *h,
                                            NVTegraH264DecodeContext *ctx)
{
    const PPS *pps = h->ps.pps;
    const SPS *sps = h->ps.sps;

    H264Picture *refs[16+1] = {0};
    NVTegraH264FrameData *fr_priv;
    int num_refs, max, i, diff, err, slot;

    if (!pps || !sps || !nvtegra_h264_picture_usable(h->cur_pic_ptr)) {
        av_log(avctx, AV_LOG_ERROR,
               "nvtegra/h264: dropping frame: current picture state is not usable\n");
        return AVERROR_INVALIDDATA;
    }

    *setup = (nvdec_h264_pic_s){
        .mbhist_buffer_size                     = ctx->mbhist_size,

        .gptimer_timeout_value                  = 0, /* Default value */

        .log2_max_pic_order_cnt_lsb_minus4      = FFMAX(sps->log2_max_poc_lsb - 4, 0),
        .delta_pic_order_always_zero_flag       = sps->delta_pic_order_always_zero_flag,
        .frame_mbs_only_flag                    = sps->frame_mbs_only_flag,

        .PicWidthInMbs                          = h->mb_width,
        .FrameHeightInMbs                       = h->mb_height,

        .tileFormat                             = 0, /* TBL */
        .gob_height                             = 0, /* GOB_2 */

        .entropy_coding_mode_flag               = pps->cabac,
        .pic_order_present_flag                 = pps->pic_order_present,
        .num_ref_idx_l0_active_minus1           = pps->ref_count[0] - 1,
        .num_ref_idx_l1_active_minus1           = pps->ref_count[1] - 1,
        .deblocking_filter_control_present_flag = pps->deblocking_filter_parameters_present,
        .redundant_pic_cnt_present_flag         = pps->redundant_pic_cnt_present,
        .transform_8x8_mode_flag                = pps->transform_8x8_mode,

        .pitch_luma                             = h->cur_pic_ptr->f->linesize[0],
        .pitch_chroma                           = h->cur_pic_ptr->f->linesize[1],

        .luma_top_offset                        = 0,
        .luma_bot_offset                        = 0,
        .luma_frame_offset                      = 0,
        .chroma_top_offset                      = 0,
        .chroma_bot_offset                      = 0,
        .chroma_frame_offset                    = 0,

        .HistBufferSize                         = ctx->history_size / 256,

        .MbaffFrameFlag                         = sps->mb_aff && !FIELD_PICTURE(h),
        .direct_8x8_inference_flag              = sps->direct_8x8_inference_flag,
        .weighted_pred_flag                     = pps->weighted_pred,
        .constrained_intra_pred_flag            = pps->constrained_intra_pred,
        .ref_pic_flag                           = h->nal_ref_idc != 0,
        .field_pic_flag                         = FIELD_PICTURE(h),
        .bottom_field_flag                      = h->picture_structure == PICT_BOTTOM_FIELD,
        .second_field                           = FIELD_PICTURE(h) && !h->first_field,
        .log2_max_frame_num_minus4              = sps->log2_max_frame_num - 4,
        .chroma_format_idc                      = sps->chroma_format_idc,
        .pic_order_cnt_type                     = sps->poc_type,
        .pic_init_qp_minus26                    = pps->init_qp - 26,
        .chroma_qp_index_offset                 = pps->chroma_qp_index_offset[0],
        .second_chroma_qp_index_offset          = pps->chroma_qp_index_offset[1],

        .weighted_bipred_idc                    = pps->weighted_bipred_idc,
        .frame_num                              = h->cur_pic_ptr->frame_num,
        .output_memory_layout                   = 0, /* NV12 */

        .CurrFieldOrderCnt                      = {
            field_poc(h->cur_pic_ptr->field_poc, true),
            field_poc(h->cur_pic_ptr->field_poc, false),
        },

        .lossless_ipred8x8_filter_enable        = true,
        .qpprime_y_zero_transform_bypass_flag   = sps->transform_bypass,
    };

    num_refs = 0;
    max = FFMIN(h->short_ref_count, 16);
    for (i = 0; i < max; ++i)
        nvtegra_h264_add_ref(avctx, refs, &num_refs, FF_ARRAY_ELEMS(refs),
                             h->short_ref[i], "short", i);

    for (i = 0; i < 16; ++i) {
        if (h->long_ref[i])
            nvtegra_h264_add_ref(avctx, refs, &num_refs, FF_ARRAY_ELEMS(refs),
                                 h->long_ref[i], "long", i);
    }

    for (i = 0; i < num_refs; ++i) {
        fr_priv = refs[i]->hwaccel_picture_private;
        if (!fr_priv->dpb_initialized)
            continue;

        err = register_ref(avctx, ctx, refs[i]);
        if (err < 0)
            return err;
    }

    for (i = 0; i < num_refs; ++i) {
        fr_priv = refs[i]->hwaccel_picture_private;
        if (fr_priv->dpb_initialized || !fr_priv->pic_initialized)
            continue;

        slot = nvtegra_h264_find_slot(avctx, &ctx->dpb_mask, "DPB");
        if (slot < 0)
            return slot;

        fr_priv->dpb_idx         = slot;
        fr_priv->dpb_initialized = true;

        err = register_ref(avctx, ctx, refs[i]);
        if (err < 0)
            return err;
    }

    fr_priv = h->cur_pic_ptr->hwaccel_picture_private;
    if (!fr_priv->pic_initialized) {
        slot = nvtegra_h264_find_slot(avctx, &ctx->pic_idx_mask, "picture");
        if (slot < 0)
            return slot;

        *fr_priv = (NVTegraH264FrameData){
            .pic_idx         = slot,
            .pic_initialized = true,
        };
    }

    if (fr_priv->pic_idx >= 16) {
        av_log(avctx, AV_LOG_ERROR,
               "nvtegra/h264: dropping frame: current picture index out of range (%u)\n",
               fr_priv->pic_idx);
        return AVERROR_INVALIDDATA;
    }

    setup->CurrPicIdx = setup->CurrColIdx = fr_priv->pic_idx;

    for (i = 0; i < FF_ARRAY_ELEMS(setup->dpb); ++i) {
        if (ctx->dpb_mask & (1u << i)) {
            err = dpb_add(avctx, &setup->dpb[i], ctx->dpb[i]);
            if (err < 0)
                return err;
        }
    }

    memcpy(setup->WeightScale,       pps->scaling_matrix4,    sizeof(setup->WeightScale));
    memcpy(setup->WeightScale8x8[0], pps->scaling_matrix8[0], sizeof(setup->WeightScale8x8[0]));
    memcpy(setup->WeightScale8x8[1], pps->scaling_matrix8[3], sizeof(setup->WeightScale8x8[1]));

    diff = INT_MAX;
    ctx->scratch_ref = h->cur_pic_ptr;
    for (i = 0; i < FF_ARRAY_ELEMS(ctx->dpb); ++i) {
        if (!(ctx->dpb_mask & (1u << i)) || !nvtegra_h264_picture_usable(ctx->dpb[i]))
            continue;

        if (FFABS(h->cur_pic_ptr->frame_num - ctx->dpb[i]->frame_num) < diff) {
            diff = FFABS(h->cur_pic_ptr->frame_num - ctx->dpb[i]->frame_num);
            ctx->scratch_ref = ctx->dpb[i];
        }
    }

    return 0;
}
'''

PREPARE_CMDBUF = r'''
static int nvtegra_h264_prepare_cmdbuf(AVNVTegraCmdbuf *cmdbuf, H264Context *h,
                                       AVFrame *cur_frame, NVTegraH264DecodeContext *ctx)
{
    FrameDecodeData     *fdd;
    FFNVTegraDecodeFrame *tf;
    AVNVTegraMap  *input_map;

    H264Picture *refs[16+1];
    NVTegraH264FrameData *fr_priv;
    int err, i;

    if (!cur_frame || !cur_frame->private_ref) {
        av_log(NULL, AV_LOG_ERROR,
               "nvtegra/h264: cannot build command buffer: current frame state is missing\n");
        return AVERROR_INVALIDDATA;
    }

    fdd = (FrameDecodeData *)cur_frame->private_ref->data;
    if (!fdd || !fdd->hwaccel_priv) {
        av_log(NULL, AV_LOG_ERROR,
               "nvtegra/h264: cannot build command buffer: hwaccel state is missing\n");
        return AVERROR_INVALIDDATA;
    }

    tf = fdd->hwaccel_priv;
    if (!tf->input_map_ref || !tf->input_map_ref->data) {
        av_log(NULL, AV_LOG_ERROR,
               "nvtegra/h264: cannot build command buffer: input map is missing\n");
        return AVERROR_INVALIDDATA;
    }

    input_map = (AVNVTegraMap *)tf->input_map_ref->data;

    if (!nvtegra_h264_picture_usable(ctx->scratch_ref)) {
        if (nvtegra_h264_picture_usable(h->cur_pic_ptr)) {
            ctx->scratch_ref = h->cur_pic_ptr;
        } else {
            av_log(NULL, AV_LOG_ERROR,
                   "nvtegra/h264: cannot build command buffer: no usable scratch reference\n");
            return AVERROR_INVALIDDATA;
        }
    }

    err = av_nvtegra_cmdbuf_begin(cmdbuf, HOST1X_CLASS_NVDEC);
    if (err < 0)
        return err;

    AV_NVTEGRA_PUSH_VALUE(cmdbuf, NVC5B0_SET_APPLICATION_ID,
                          AV_NVTEGRA_ENUM(NVC5B0_SET_APPLICATION_ID, ID, H264));
    AV_NVTEGRA_PUSH_VALUE(cmdbuf, NVC5B0_SET_CONTROL_PARAMS,
                          AV_NVTEGRA_ENUM (NVC5B0_SET_CONTROL_PARAMS, CODEC_TYPE,     H264) |
                          AV_NVTEGRA_VALUE(NVC5B0_SET_CONTROL_PARAMS, ERR_CONCEAL_ON, 1)    |
                          AV_NVTEGRA_VALUE(NVC5B0_SET_CONTROL_PARAMS, GPTIMER_ON,     1));
    AV_NVTEGRA_PUSH_VALUE(cmdbuf, NVC5B0_SET_PICTURE_INDEX,
                          AV_NVTEGRA_VALUE(NVC5B0_SET_PICTURE_INDEX, INDEX, ctx->core.frame_idx));

    AV_NVTEGRA_PUSH_RELOC(cmdbuf, NVC5B0_SET_DRV_PIC_SETUP_OFFSET,
                          input_map,        ctx->core.pic_setup_off,     NVHOST_RELOC_TYPE_DEFAULT);
    AV_NVTEGRA_PUSH_RELOC(cmdbuf, NVC5B0_SET_IN_BUF_BASE_OFFSET,
                          input_map,        ctx->core.bitstream_off,     NVHOST_RELOC_TYPE_DEFAULT);
    AV_NVTEGRA_PUSH_RELOC(cmdbuf, NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET,
                          input_map,        ctx->core.slice_offsets_off, NVHOST_RELOC_TYPE_DEFAULT);
    AV_NVTEGRA_PUSH_RELOC(cmdbuf, NVC5B0_SET_NVDEC_STATUS_OFFSET,
                          input_map,        ctx->core.status_off,        NVHOST_RELOC_TYPE_DEFAULT);

    AV_NVTEGRA_PUSH_RELOC(cmdbuf, NVC5B0_SET_COLOC_DATA_OFFSET,
                          &ctx->common_map, ctx->coloc_off,              NVHOST_RELOC_TYPE_DEFAULT);
    AV_NVTEGRA_PUSH_RELOC(cmdbuf, NVC5B0_H264_SET_MBHIST_BUF_OFFSET,
                          &ctx->common_map, ctx->mbhist_off,             NVHOST_RELOC_TYPE_DEFAULT);
    AV_NVTEGRA_PUSH_RELOC(cmdbuf, NVC5B0_SET_HISTORY_OFFSET,
                          &ctx->common_map, ctx->history_off,            NVHOST_RELOC_TYPE_DEFAULT);

    for (i = 0; i < FF_ARRAY_ELEMS(refs); ++i)
        refs[i] = ctx->scratch_ref;

    if (!nvtegra_h264_picture_usable(h->cur_pic_ptr)) {
        av_log(NULL, AV_LOG_ERROR,
               "nvtegra/h264: cannot build command buffer: current picture is unusable\n");
        return AVERROR_INVALIDDATA;
    }

    fr_priv = h->cur_pic_ptr->hwaccel_picture_private;
    if (!fr_priv || fr_priv->pic_idx >= FF_ARRAY_ELEMS(refs)) {
        av_log(NULL, AV_LOG_ERROR,
               "nvtegra/h264: cannot build command buffer: current picture index is invalid\n");
        return AVERROR_INVALIDDATA;
    }
    refs[fr_priv->pic_idx] = h->cur_pic_ptr;

    for (i = 0; i < FF_ARRAY_ELEMS(ctx->dpb); ++i) {
        if (!(ctx->dpb_mask & (1u << i)))
            continue;
        if (!nvtegra_h264_picture_usable(ctx->dpb[i])) {
            av_log(NULL, AV_LOG_WARNING,
                   "nvtegra/h264: skipping unusable DPB command reference %d\n", i);
            continue;
        }
        fr_priv = ctx->dpb[i]->hwaccel_picture_private;
        if (fr_priv->pic_idx >= FF_ARRAY_ELEMS(refs)) {
            av_log(NULL, AV_LOG_WARNING,
                   "nvtegra/h264: skipping DPB command reference %d with invalid pic_idx=%u\n",
                   i, fr_priv->pic_idx);
            continue;
        }
        refs[fr_priv->pic_idx] = ctx->dpb[i];
    }

    for (i = 0; i < FF_ARRAY_ELEMS(refs); ++i) {
        AVFrame *fr;
        AVNVTegraMap *map;
        uintptr_t luma, chroma;

        if (!nvtegra_h264_picture_usable(refs[i])) {
            av_log(NULL, AV_LOG_ERROR,
                   "nvtegra/h264: cannot submit command buffer: picture slot %d is unusable\n",
                   i);
            return AVERROR_INVALIDDATA;
        }

        fr = refs[i]->f;
        map = av_nvtegra_frame_get_fbuf_map(fr);
        luma = (uintptr_t)fr->data[0];
        chroma = (uintptr_t)fr->data[1];
        if (chroma < luma) {
            av_log(NULL, AV_LOG_ERROR,
                   "nvtegra/h264: cannot submit command buffer: invalid chroma offset at slot %d\n",
                   i);
            return AVERROR_INVALIDDATA;
        }

        AV_NVTEGRA_PUSH_RELOC(cmdbuf, NVC5B0_SET_PICTURE_LUMA_OFFSET0   + i * 4,
                              map, 0, NVHOST_RELOC_TYPE_DEFAULT);
        AV_NVTEGRA_PUSH_RELOC(cmdbuf, NVC5B0_SET_PICTURE_CHROMA_OFFSET0 + i * 4,
                              map, chroma - luma, NVHOST_RELOC_TYPE_DEFAULT);
    }

    AV_NVTEGRA_PUSH_VALUE(cmdbuf, NVC5B0_EXECUTE,
                          AV_NVTEGRA_ENUM(NVC5B0_EXECUTE, AWAKEN, ENABLE));

    err = av_nvtegra_cmdbuf_end(cmdbuf);
    if (err < 0)
        return err;

    return 0;
}
'''

START_FRAME = r'''
static int nvtegra_h264_start_frame(AVCodecContext *avctx, const uint8_t *buf, uint32_t buf_size) {
    H264Context                *h = avctx->priv_data;
    NVTegraH264DecodeContext *ctx = avctx->internal->hwaccel_priv_data;
    AVFrame                *frame;
    FrameDecodeData          *fdd;
    FFNVTegraDecodeFrame *tf;
    AVNVTegraMap *input_map;
    uint8_t *mem;
    int err;

    av_log(avctx, AV_LOG_DEBUG, "Starting H264-NVTEGRA frame with pixel format %s\n",
           av_get_pix_fmt_name(avctx->sw_pix_fmt));

    if (!h || !h->cur_pic_ptr || !h->cur_pic_ptr->f) {
        av_log(avctx, AV_LOG_ERROR,
               "nvtegra/h264: cannot start frame: current picture/frame is missing\n");
        return AVERROR_INVALIDDATA;
    }

    frame = h->cur_pic_ptr->f;
    err = ff_nvtegra_start_frame(avctx, frame, &ctx->core);
    if (err < 0)
        return err;

    if (!frame->private_ref || !frame->private_ref->data) {
        av_log(avctx, AV_LOG_ERROR,
               "nvtegra/h264: cannot start frame: frame private state is missing\n");
        return AVERROR_INVALIDDATA;
    }

    fdd = (FrameDecodeData *)frame->private_ref->data;
    tf = fdd->hwaccel_priv;
    if (!tf || !tf->input_map_ref || !tf->input_map_ref->data) {
        av_log(avctx, AV_LOG_ERROR,
               "nvtegra/h264: cannot start frame: input map state is missing\n");
        return AVERROR_INVALIDDATA;
    }

    input_map = (AVNVTegraMap *)tf->input_map_ref->data;
    mem = av_nvtegra_map_get_addr(input_map);
    if (!mem) {
        av_log(avctx, AV_LOG_ERROR,
               "nvtegra/h264: cannot start frame: input map address is NULL\n");
        return AVERROR_INVALIDDATA;
    }

    memset(ctx->dpb, 0, sizeof(ctx->dpb));
    ctx->dpb_mask = ctx->pic_idx_mask = 0;

    err = nvtegra_h264_prepare_frame_setup(avctx,
                                           (nvdec_h264_pic_s *)(mem + ctx->core.pic_setup_off),
                                           h, ctx);
    if (err < 0)
        return err;

    return 0;
}
'''

END_FRAME = r'''
static int nvtegra_h264_end_frame(AVCodecContext *avctx) {
    H264Context                *h = avctx->priv_data;
    NVTegraH264DecodeContext *ctx = avctx->internal->hwaccel_priv_data;
    AVFrame                *frame;
    FrameDecodeData          *fdd;
    FFNVTegraDecodeFrame      *tf;

    nvdec_h264_pic_s *setup;
    AVNVTegraMap *input_map;
    uint8_t *mem;
    int err;

    av_log(avctx, AV_LOG_DEBUG, "Ending H264-NVTEGRA frame with %u slices -> %u bytes\n",
           ctx->core.num_slices, ctx->core.bitstream_len);

    if (!h || !h->cur_pic_ptr || !h->cur_pic_ptr->f) {
        av_log(avctx, AV_LOG_ERROR,
               "nvtegra/h264: cannot end frame: current picture/frame is missing\n");
        return AVERROR_INVALIDDATA;
    }

    frame = h->cur_pic_ptr->f;
    if (!frame->private_ref || !frame->private_ref->data) {
        av_log(avctx, AV_LOG_ERROR,
               "nvtegra/h264: cannot end frame: frame private state is missing\n");
        return AVERROR_INVALIDDATA;
    }

    fdd = (FrameDecodeData *)frame->private_ref->data;
    tf = fdd->hwaccel_priv;

    if (!tf || !ctx->core.num_slices)
        return 0;

    if (!tf->input_map_ref || !tf->input_map_ref->data) {
        av_log(avctx, AV_LOG_ERROR,
               "nvtegra/h264: cannot end frame: input map state is missing\n");
        return AVERROR_INVALIDDATA;
    }

    input_map = (AVNVTegraMap *)tf->input_map_ref->data;
    mem = av_nvtegra_map_get_addr(input_map);
    if (!mem) {
        av_log(avctx, AV_LOG_ERROR,
               "nvtegra/h264: cannot end frame: input map address is NULL\n");
        return AVERROR_INVALIDDATA;
    }

    setup = (nvdec_h264_pic_s *)(mem + ctx->core.pic_setup_off);
    setup->stream_len  = ctx->core.bitstream_len + sizeof(bitstream_end_sequence);
    setup->slice_count = ctx->core.num_slices;

    err = nvtegra_h264_prepare_cmdbuf(&ctx->core.cmdbuf, h, frame, ctx);
    if (err < 0)
        return err;

    return ff_nvtegra_end_frame(avctx, frame, &ctx->core, bitstream_end_sequence,
                                sizeof(bitstream_end_sequence));
}
'''


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: twinx_patch_nvtegra_h264_refs.py <nvtegra_h264.c>")

    path = Path(sys.argv[1])
    text = path.read_text()
    if MARKER in text:
        print("twiNX H264 reference/frame guard already applied")
        return

    text = text.replace("#include <stdbool.h>\n", "#include <stdbool.h>\n#include <stdint.h>\n")
    anchor = "static inline int field_poc(int poc[2], bool top) {\n    return (poc[!top] != INT_MAX) ? poc[!top] : 0;\n}\n"
    if anchor not in text:
        raise RuntimeError("field_poc anchor not found")
    text = text.replace(anchor, anchor + HELPERS + "\n")

    text = replace_function(text, "static void dpb_add(", DPB_ADD)
    text = replace_function(text, "static inline void register_ref(", REGISTER_REF)
    text = replace_function(text, "static inline int find_slot(", FIND_SLOT)
    text = replace_function(text, "static void nvtegra_h264_prepare_frame_setup(", PREPARE_FRAME_SETUP)
    text = replace_function(text, "static int nvtegra_h264_prepare_cmdbuf(", PREPARE_CMDBUF)
    text = replace_function(text, "static int nvtegra_h264_start_frame(", START_FRAME)
    text = replace_function(text, "static int nvtegra_h264_end_frame(", END_FRAME)

    path.write_text(text)
    print("Applied twiNX H264 NVTEGRA reference/frame guard")


if __name__ == "__main__":
    main()
