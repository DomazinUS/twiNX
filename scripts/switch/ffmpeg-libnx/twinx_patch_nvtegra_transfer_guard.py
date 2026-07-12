#!/usr/bin/env python3
from pathlib import Path
import sys

MARKER = "twiNX NVTEGRA transfer guard"


def find_function_end(text, start):
    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError("function body start not found")
    depth = 0
    i = brace
    in_string = in_char = in_line_comment = in_block_comment = escape = False
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
/* twiNX NVTEGRA transfer guard:
 * Twitch HLS discontinuities can leave a hardware frame in a state that is
 * still structurally present but unsafe to download. Validate maps, planes and
 * backing buffers before VIC or CPU transfer code dereferences them.
 */
static AVNVTegraMap *nvtegra_checked_frame_map(void *logctx, const AVFrame *frame,
                                               const char *role)
{
    AVNVTegraFrame *nvframe;
    AVNVTegraMap *map;

    if (!frame || frame->format != AV_PIX_FMT_NVTEGRA || !frame->buf[0] ||
        !frame->buf[0]->data) {
        av_log(logctx, AV_LOG_ERROR,
               "nvtegra/transfer: invalid %s hardware frame container "
               "(frame=%p format=%d buf0=%p)\n",
               role, frame, frame ? frame->format : -1,
               frame ? frame->buf[0] : NULL);
        return NULL;
    }

    nvframe = (AVNVTegraFrame *)frame->buf[0]->data;
    if (!nvframe->map_ref || !nvframe->map_ref->data) {
        av_log(logctx, AV_LOG_ERROR,
               "nvtegra/transfer: invalid %s hardware frame map "
               "(nvframe=%p map_ref=%p)\n",
               role, nvframe, nvframe ? nvframe->map_ref : NULL);
        return NULL;
    }

    map = (AVNVTegraMap *)nvframe->map_ref->data;
    if (!av_nvtegra_map_get_addr(map) || !av_nvtegra_map_get_size(map)) {
        av_log(logctx, AV_LOG_ERROR,
               "nvtegra/transfer: invalid %s hardware map address/size "
               "(map=%p addr=%p size=%zu)\n",
               role, map, av_nvtegra_map_get_addr(map),
               av_nvtegra_map_get_size(map));
        return NULL;
    }

    return map;
}

static int nvtegra_validate_hw_planes(void *logctx, const AVFrame *frame,
                                      int num_planes, const char *role)
{
    AVNVTegraMap *map;
    uint8_t *base;
    size_t size;
    int i;

    map = nvtegra_checked_frame_map(logctx, frame, role);
    if (!map)
        return AVERROR_INVALIDDATA;

    base = av_nvtegra_map_get_addr(map);
    size = av_nvtegra_map_get_size(map);
    for (i = 0; i < num_planes; ++i) {
        uintptr_t ptr = (uintptr_t)frame->data[i];
        uintptr_t start = (uintptr_t)base;
        uintptr_t end = start + size;

        if (!frame->data[i] || frame->linesize[i] <= 0 || ptr < start || ptr >= end) {
            av_log(logctx, AV_LOG_ERROR,
                   "nvtegra/transfer: invalid %s plane %d "
                   "(data=%p linesize=%d map=[%p,%p))\n",
                   role, i, frame->data[i], frame->linesize[i], base, base + size);
            return AVERROR_INVALIDDATA;
        }
    }

    return 0;
}

static int nvtegra_find_sw_plane_buffer(void *logctx, const AVFrame *frame,
                                        int plane, uint8_t **base,
                                        AVBufferRef **buf)
{
    int j;

    if (!frame->data[plane] || frame->linesize[plane] <= 0) {
        av_log(logctx, AV_LOG_ERROR,
               "nvtegra/transfer: invalid software plane %d "
               "(data=%p linesize=%d)\n",
               plane, frame->data[plane], frame->linesize[plane]);
        return AVERROR_INVALIDDATA;
    }

    for (j = 0; j < FF_ARRAY_ELEMS(frame->buf); ++j) {
        uint8_t *start, *end;

        if (!frame->buf[j] || !frame->buf[j]->data || !frame->buf[j]->size)
            continue;

        start = frame->buf[j]->data;
        end = start + frame->buf[j]->size;
        if (start <= frame->data[plane] && frame->data[plane] < end) {
            *base = (uint8_t *)((uintptr_t)start & ~0xfff);
            *buf = frame->buf[j];
            return 0;
        }
    }

    av_log(logctx, AV_LOG_ERROR,
           "nvtegra/transfer: software plane %d is not backed by an AVBuffer "
           "(data=%p)\n",
           plane, frame->data[plane]);
    return AVERROR_INVALIDDATA;
}
'''

CPU_TRANSFER = r'''
static int nvtegra_cpu_transfer_data(AVHWFramesContext *ctx, const AVFrame *dst, const AVFrame *src,
                                     int num_planes, bool from)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(ctx->sw_format);
    const AVFrame *hwframe, *swframe;
    AVNVTegraMap *map;
    int h, i, err;

    hwframe = from ? src : dst, swframe = from ? dst : src;

    err = nvtegra_validate_hw_planes(ctx, hwframe, num_planes,
                                     from ? "source" : "destination");
    if (err < 0)
        return err;

    map = nvtegra_checked_frame_map(ctx, hwframe, from ? "source" : "destination");
    if (!map)
        return AVERROR_INVALIDDATA;

    if (swframe->format != ctx->sw_format) {
        av_log(ctx, AV_LOG_ERROR, "Source and destination must have the same format for cpu transfers\n");
        return AVERROR(EINVAL);
    }

    for (i = 0; i < num_planes; ++i) {
        uint8_t *base = NULL;
        AVBufferRef *buf = NULL;
        err = nvtegra_find_sw_plane_buffer(ctx, swframe, i, &base, &buf);
        if (err < 0)
            return err;
    }

    if (from) {
        av_nvtegra_map_cache_op(map, NVMAP_CACHE_OP_INV,
                                av_nvtegra_map_get_addr(map), av_nvtegra_map_get_size(map));
    }

    h = FFALIGN(dst->height, 2);

    for (i = 0; i < num_planes; ++i) {
        if (map->is_linear) {
            av_image_copy_plane(dst->data[i], dst->linesize[i], src->data[i], src->linesize[i],
                                FFMIN(dst->linesize[i], src->linesize[i]),
                                h >> (i ? desc->log2_chroma_h : 0));
        } else {
            if (from)
                nvtegra_cpu_copy_plane(dst->data[i], dst->linesize[i], src->data[i], src->linesize[i],
                                       h >> (i ? desc->log2_chroma_h : 0), true);
            else
                nvtegra_cpu_copy_plane(dst->data[i], dst->linesize[i], src->data[i], src->linesize[i],
                                       h >> (i ? desc->log2_chroma_h : 0), false);
        }
    }

    if (!from) {
        av_nvtegra_map_cache_op(map, NVMAP_CACHE_OP_WB,
                                av_nvtegra_map_get_addr(map), av_nvtegra_map_get_size(map));
    }

    return 0;
}
'''

VIC_PREPARE_CMDBUF = r'''
static int nvtegra_vic_prepare_cmdbuf(AVHWFramesContext *ctx, AVNVTegraJobPool *pool, AVNVTegraJob *job,
                                      const AVFrame *src, const AVFrame *dst, enum AVPixelFormat fmt,
                                      AVNVTegraMap **plane_maps, uint32_t *plane_offsets, int num_planes)
{
    NVTegraDevicePriv *priv = ctx->device_ctx->hwctx;
    AVNVTegraCmdbuf *cmdbuf = &job->cmdbuf;

    AVNVTegraMap *src_maps[4] = {0}, *dst_maps[4] = {0};
    uint32_t src_map_offsets[4] = {0}, dst_map_offsets[4] = {0};
    int src_reloc_type, dst_reloc_type, i, err;

#define RELOC_VARS(frame) ({                                                                  \
    if (frame->format == AV_PIX_FMT_NVTEGRA) {                                                \
        AVNVTegraMap *fmap = nvtegra_checked_frame_map(ctx, frame, #frame);                   \
        if (!fmap)                                                                            \
            return AVERROR_INVALIDDATA;                                                       \
        for (i = 0; i < FF_ARRAY_ELEMS(AV_JOIN(frame, _map_offsets)); ++i) {                  \
            uintptr_t base = (uintptr_t)frame->data[0];                                       \
            uintptr_t ptr = (uintptr_t)frame->data[i];                                        \
            AV_JOIN(frame, _maps)[i] = fmap;                                                  \
            /* NV12 uses only data[0] and data[1]. Unused AVFrame plane slots are             \
             * legitimately NULL and must not reject an otherwise valid hardware frame. */   \
            if (!frame->data[i]) {                                                            \
                AV_JOIN(frame, _map_offsets)[i] = 0;                                          \
                continue;                                                                     \
            }                                                                                 \
            if (ptr < base) {                                                                 \
                av_log(ctx, AV_LOG_ERROR,                                                     \
                       "nvtegra/transfer: invalid " #frame " used-plane offset %d\n", i);   \
                return AVERROR_INVALIDDATA;                                                   \
            }                                                                                 \
            AV_JOIN(frame, _map_offsets)[i] = ptr - base;                                     \
        }                                                                                     \
        AV_JOIN(frame, _reloc_type) = !fmap->is_linear ?                                      \
            NVHOST_RELOC_TYPE_BLOCK_LINEAR : NVHOST_RELOC_TYPE_PITCH_LINEAR;                  \
    } else {                                                                                  \
        for (i = 0; i < FF_ARRAY_ELEMS(AV_JOIN(frame, _map_offsets)); ++i) {                  \
            AV_JOIN(frame, _maps       )[i] = plane_maps   [i];                               \
            AV_JOIN(frame, _map_offsets)[i] = plane_offsets[i];                               \
        }                                                                                     \
        AV_JOIN(frame, _reloc_type) = NVHOST_RELOC_TYPE_PITCH_LINEAR;                         \
    }                                                                                         \
})

    RELOC_VARS(src);
    RELOC_VARS(dst);

    err = av_nvtegra_cmdbuf_begin(cmdbuf, HOST1X_CLASS_VIC);
    if (err < 0)
        return err;

    AV_NVTEGRA_PUSH_VALUE(cmdbuf, NVB0B6_VIDEO_COMPOSITOR_SET_CONTROL_PARAMS,
                          AV_NVTEGRA_VALUE(NVB0B6_VIDEO_COMPOSITOR_SET_CONTROL_PARAMS, CONFIG_STRUCT_SIZE, sizeof(VicConfigStruct) >> 4) |
                          AV_NVTEGRA_VALUE(NVB0B6_VIDEO_COMPOSITOR_SET_CONTROL_PARAMS, GPTIMER_ON,         1)                            |
                          AV_NVTEGRA_VALUE(NVB0B6_VIDEO_COMPOSITOR_SET_CONTROL_PARAMS, FALCON_CONTROL,     1));
    AV_NVTEGRA_PUSH_RELOC(cmdbuf, NVB0B6_VIDEO_COMPOSITOR_SET_CONFIG_STRUCT_OFFSET,
                          &job->input_map, priv->vic_setup_off, NVHOST_RELOC_TYPE_DEFAULT);

    switch (fmt) {
        case AV_PIX_FMT_RGB565:
            AV_NVTEGRA_PUSH_RELOC(cmdbuf, NVB0B6_VIDEO_COMPOSITOR_SET_SURFACE0_LUMA_OFFSET(0),
                                  src_maps[0], src_map_offsets[0], src_reloc_type);
            AV_NVTEGRA_PUSH_RELOC(cmdbuf, NVB0B6_VIDEO_COMPOSITOR_SET_OUTPUT_SURFACE_LUMA_OFFSET,
                                  dst_maps[0], dst_map_offsets[0], dst_reloc_type);
            break;
        case AV_PIX_FMT_RGB32:
            if (!src->data[1] || !dst->data[1]) {
                av_log(ctx, AV_LOG_ERROR,
                       "nvtegra/transfer: missing chroma plane for RGB32 transfer\n");
                return AVERROR_INVALIDDATA;
            }
            AV_NVTEGRA_PUSH_RELOC(cmdbuf, NVB0B6_VIDEO_COMPOSITOR_SET_SURFACE0_LUMA_OFFSET(0),
                                  src_maps[1], src_map_offsets[1], src_reloc_type);
            AV_NVTEGRA_PUSH_RELOC(cmdbuf, NVB0B6_VIDEO_COMPOSITOR_SET_OUTPUT_SURFACE_LUMA_OFFSET,
                                  dst_maps[1], dst_map_offsets[1], dst_reloc_type);
            break;

        case AV_PIX_FMT_GRAY8:
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_YUV420P:
            for (i = 0; i < num_planes; ++i) {
                if (!src_maps[i] || !dst_maps[i]) {
                    av_log(ctx, AV_LOG_ERROR,
                           "nvtegra/transfer: missing VIC map for plane %d\n", i);
                    return AVERROR_INVALIDDATA;
                }
                AV_NVTEGRA_PUSH_RELOC(cmdbuf, NVB0B6_VIDEO_COMPOSITOR_SET_SURFACE0_LUMA_OFFSET(0)    + i * sizeof(uint32_t),
                                      src_maps[i], src_map_offsets[i], src_reloc_type);
                AV_NVTEGRA_PUSH_RELOC(cmdbuf, NVB0B6_VIDEO_COMPOSITOR_SET_OUTPUT_SURFACE_LUMA_OFFSET + i * sizeof(uint32_t),
                                      dst_maps[i], dst_map_offsets[i], dst_reloc_type);
            }
            break;
        default:
            return AVERROR(EINVAL);
    }

    AV_NVTEGRA_PUSH_VALUE(cmdbuf, NVB0B6_VIDEO_COMPOSITOR_EXECUTE,
                          AV_NVTEGRA_ENUM(NVB0B6_VIDEO_COMPOSITOR_EXECUTE, AWAKEN, ENABLE));

    err = av_nvtegra_cmdbuf_add_syncpt_incr(cmdbuf, pool->channel->syncpt, 0);
    if (err < 0)
        return err;

    err = av_nvtegra_cmdbuf_end(cmdbuf);
    if (err < 0)
        return err;

    return 0;
}
'''

VIC_TRANSFER = r'''
static int nvtegra_vic_transfer_data(AVHWFramesContext *ctx, const AVFrame *dst, const AVFrame *src,
                                     int num_planes, bool from)
{
    NVTegraDevicePriv       *priv = ctx->device_ctx->hwctx;
    AVNVTegraDeviceContext *hwctx = &priv->p;

    AVBufferRef *job_ref = NULL;
    AVNVTegraJob *job;
    const AVFrame *hwframe, *swframe;
    uint8_t *map_bases[4] = {0};
    AVBufferRef *map_bufs[4] = {0};
    AVNVTegraMap maps[4] = {0};
    AVNVTegraMap *plane_maps[4] = {0};
    uint32_t plane_offsets[4] = {0};
    int i, err;

    hwframe = from ? src : dst;
    swframe = from ? dst : src;

    err = nvtegra_validate_hw_planes(ctx, hwframe, num_planes,
                                     from ? "source" : "destination");
    if (err < 0)
        goto fail;

    if (swframe->format != ctx->sw_format) {
        av_log(ctx, AV_LOG_ERROR,
               "nvtegra/transfer: software frame format mismatch (%d != %d)\n",
               swframe->format, ctx->sw_format);
        err = AVERROR(EINVAL);
        goto fail;
    }

    for (i = 0; i < num_planes; ++i) {
        err = nvtegra_find_sw_plane_buffer(ctx, swframe, i,
                                           &map_bases[i], &map_bufs[i]);
        if (err < 0)
            goto fail;
    }

    job_ref = av_nvtegra_job_pool_get(&priv->job_pool);
    if (!job_ref) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    job = (AVNVTegraJob *)job_ref->data;

    for (i = 0; i < FF_ARRAY_ELEMS(maps); ++i) {
        if (!swframe->buf[i])
            continue;
        if (!swframe->buf[i]->data || !swframe->buf[i]->size) {
            av_log(ctx, AV_LOG_ERROR,
                   "nvtegra/transfer: invalid software buffer %d for VIC map\n", i);
            err = AVERROR_INVALIDDATA;
            goto fail;
        }

        map_bases[i] = (uint8_t *)((uintptr_t)swframe->buf[i]->data & ~0xfff);
        err = av_nvtegra_map_from_va(&maps[i], &hwctx->vic_channel, map_bases[i],
                                     swframe->buf[i]->size + ((uintptr_t)swframe->buf[i]->data & 0xfff),
                                     0x100, NVMAP_HANDLE_CACHEABLE);
        if (err < 0)
            goto fail;

        err = av_nvtegra_map_map(&maps[i]);
        if (err < 0)
            goto fail;

        av_nvtegra_map_cache_op(&maps[i], NVMAP_CACHE_OP_WB_INV,
                                ((uint8_t *)av_nvtegra_map_get_addr(&maps[i])) +
                                    ((uintptr_t)swframe->buf[i]->data & 0xfff),
                                swframe->buf[i]->size);
    }

    for (i = 0; i < num_planes; ++i) {
        int j, found = 0;
        for (j = 0; j < FF_ARRAY_ELEMS(swframe->buf); ++j) {
            if (!swframe->buf[j] || !swframe->buf[j]->data || !swframe->buf[j]->size)
                continue;

            if ((swframe->buf[j]->data <= swframe->data[i]) &&
                    (swframe->data[i] < swframe->buf[j]->data + swframe->buf[j]->size)) {
                plane_maps   [i] = &maps[j];
                plane_offsets[i] = swframe->data[i] - map_bases[j];
                found = 1;
                break;
            }
        }

        if (!found || !plane_maps[i]) {
            av_log(ctx, AV_LOG_ERROR,
                   "nvtegra/transfer: cannot map software plane %d for VIC\n", i);
            err = AVERROR_INVALIDDATA;
            goto fail;
        }
    }

    if (swframe->format == AV_PIX_FMT_YUV420P) {
        FFSWAP(AVNVTegraMap *, plane_maps   [1], plane_maps   [2]);
        FFSWAP(uint32_t,       plane_offsets[1], plane_offsets[2]);
    }

    if (swframe->format == AV_PIX_FMT_P010) {
        err = nvtegra_vic_copy_plane(ctx, job, src, dst, AV_PIX_FMT_RGB565,
                                     plane_maps, plane_offsets, 1, false);
        if (err < 0)
            goto fail;

        err = nvtegra_vic_copy_plane(ctx, job, src, dst, AV_PIX_FMT_RGB32,
                                     plane_maps, plane_offsets, 1, true);
        if (err < 0)
            goto fail;
    } else {
        err = nvtegra_vic_copy_plane(ctx, job, src, dst, swframe->format,
                                     plane_maps, plane_offsets, num_planes, false);
        if (err < 0)
            goto fail;
    }

fail:
    for (i = 0; i < FF_ARRAY_ELEMS(maps); ++i) {
        av_nvtegra_map_unmap(&maps[i]);
        av_nvtegra_map_close(&maps[i]);
    }

    av_buffer_unref(&job_ref);

    return err;
}
'''

TRANSFER_DATA = r'''
static int nvtegra_transfer_data(AVHWFramesContext *ctx, AVFrame *dst, const AVFrame *src) {
    const AVFrame *swframe, *hwframe;
    bool from;
    int num_planes, i, err;

    if (!dst || !src) {
        av_log(ctx, AV_LOG_ERROR, "nvtegra/transfer: missing transfer frame\n");
        return AVERROR_INVALIDDATA;
    }

    from    = !dst->hw_frames_ctx;
    swframe = from ? dst : src;
    hwframe = from ? src : dst;

    if (swframe->hw_frames_ctx)
        return AVERROR(ENOSYS);

    if (swframe->format != ctx->sw_format) {
        av_log(ctx, AV_LOG_ERROR,
               "nvtegra/transfer: software frame format mismatch (%d != %d)\n",
               swframe->format, ctx->sw_format);
        return AVERROR(EINVAL);
    }

    num_planes = av_pix_fmt_count_planes(swframe->format);
    if (num_planes <= 0 || num_planes > 4) {
        av_log(ctx, AV_LOG_ERROR,
               "nvtegra/transfer: invalid plane count %d for format %d\n",
               num_planes, swframe->format);
        return AVERROR_INVALIDDATA;
    }

    err = nvtegra_validate_hw_planes(ctx, hwframe, num_planes,
                                     from ? "source" : "destination");
    if (err < 0)
        return err;

    for (i = 0; i < num_planes; ++i) {
        uint8_t *base = NULL;
        AVBufferRef *buf = NULL;

        err = nvtegra_find_sw_plane_buffer(ctx, swframe, i, &base, &buf);
        if (err < 0)
            return err;

        if (((uintptr_t)swframe->data[i] & 0xff) || (swframe->linesize[i] & 0xff)) {
            av_log(ctx, AV_LOG_WARNING, "Frame address/pitch not aligned to 256, "
                                        "falling back to cpu transfer\n");
            return nvtegra_cpu_transfer_data(ctx, dst, src, num_planes, from);
        }
    }

    return nvtegra_vic_transfer_data(ctx, dst, src, num_planes, from);
}
'''


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: twinx_patch_nvtegra_transfer_guard.py <hwcontext_nvtegra.c>")

    path = Path(sys.argv[1])
    text = path.read_text()
    if MARKER in text:
        print("twiNX NVTEGRA transfer guard already applied")
        return

    text = text.replace("#include <stdbool.h>\n", "#include <stdbool.h>\n#include <stdint.h>\n")
    anchor = "static void nvtegra_frame_free(void *opaque, uint8_t *data) {\n"
    idx = text.find(anchor)
    if idx < 0:
        raise RuntimeError("nvtegra_frame_free anchor not found")
    end = find_function_end(text, idx)
    text = text[:end] + "\n" + HELPERS + text[end:]

    text = replace_function(text, "static int nvtegra_cpu_transfer_data(", CPU_TRANSFER)
    text = replace_function(text, "static int nvtegra_vic_prepare_cmdbuf(", VIC_PREPARE_CMDBUF)
    text = replace_function(text, "static int nvtegra_vic_transfer_data(", VIC_TRANSFER)
    text = replace_function(text, "static int nvtegra_transfer_data(", TRANSFER_DATA)

    path.write_text(text)
    print("Applied twiNX NVTEGRA transfer guard")


if __name__ == "__main__":
    main()
