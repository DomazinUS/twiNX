#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import sys

MAX_SIDE_DATA = 64

DESTRUCTOR_SIG = "static void mp_image_destructor(void *ptr)"
NEW_REF_SIG = "struct mp_image *mp_image_new_ref(struct mp_image *img)"

NEW_DESTRUCTOR = r'''#define MP_IMAGE_MAX_FF_SIDE_DATA 64

static bool mp_image_ff_side_data_valid(const struct mp_image *mpi)
{
    return mpi->num_ff_side_data >= 0 &&
           mpi->num_ff_side_data <= MP_IMAGE_MAX_FF_SIDE_DATA &&
           (!mpi->num_ff_side_data || mpi->ff_side_data);
}

static void mp_image_destructor(void *ptr)
{
    mp_image_t *mpi = ptr;
    for (int p = 0; p < MP_MAX_PLANES; p++)
        av_buffer_unref(&mpi->bufs[p]);
    av_buffer_unref(&mpi->hwctx);
    av_buffer_unref(&mpi->icc_profile);
    av_buffer_unref(&mpi->a53_cc);
    av_buffer_unref(&mpi->dovi);
    av_buffer_unref(&mpi->film_grain);
    av_buffer_unref(&mpi->dovi_buf);

    if (mp_image_ff_side_data_valid(mpi)) {
        for (int n = 0; n < mpi->num_ff_side_data; n++)
            av_buffer_unref(&mpi->ff_side_data[n].buf);
        talloc_free(mpi->ff_side_data);
    }
}
'''

NEW_NEW_REF = r'''struct mp_image *mp_image_new_ref(struct mp_image *img)
{
    if (!img)
        return NULL;
    if (!img->bufs[0])
        return mp_image_new_copy(img);

    struct mp_image *new = talloc_ptrtype(NULL, new);
    talloc_set_destructor(new, mp_image_destructor);
    *new = *img;

    bool copy_ff_side_data = mp_image_ff_side_data_valid(img);
    new->num_ff_side_data = 0;
    new->ff_side_data = NULL;

    for (int p = 0; p < MP_MAX_PLANES; p++)
        ref_buffer(&new->bufs[p]);
    ref_buffer(&new->hwctx);
    ref_buffer(&new->icc_profile);
    ref_buffer(&new->a53_cc);
    ref_buffer(&new->dovi);
    ref_buffer(&new->film_grain);
    ref_buffer(&new->dovi_buf);

    if (copy_ff_side_data && img->num_ff_side_data) {
        new->ff_side_data = talloc_memdup(NULL, img->ff_side_data,
            img->num_ff_side_data * sizeof(img->ff_side_data[0]));
        new->num_ff_side_data = img->num_ff_side_data;
        for (int n = 0; n < new->num_ff_side_data; n++)
            ref_buffer(&new->ff_side_data[n].buf);
    }
    return new;
}
'''


def replace_function(source: str, signature: str, replacement: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise RuntimeError(f"function signature not found: {signature}")
    if source.find(signature, start + 1) >= 0:
        raise RuntimeError(f"function signature appears more than once: {signature}")

    brace = source.find("{", start)
    if brace < 0:
        raise RuntimeError(f"opening brace not found for: {signature}")

    depth = 0
    end = None
    in_string = False
    in_char = False
    escape = False
    in_line_comment = False
    in_block_comment = False

    i = brace
    while i < len(source):
        c = source[i]
        n = source[i + 1] if i + 1 < len(source) else ""

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
                    end = i + 1
                    break
        i += 1

    if end is None:
        raise RuntimeError(f"closing brace not found for: {signature}")

    # Consume exactly one following newline so the replacement owns spacing.
    if end < len(source) and source[end] == "\r":
        end += 1
    if end < len(source) and source[end] == "\n":
        end += 1

    return source[:start] + replacement + source[end:]


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: twinx_patch_mp_image.py <video/mp_image.c>", file=sys.stderr)
        return 2

    path = Path(sys.argv[1])
    if not path.is_file():
        print(f"ERROR: source file not found: {path}", file=sys.stderr)
        return 1

    text = path.read_text(encoding="utf-8")

    if "MP_IMAGE_MAX_FF_SIDE_DATA" in text:
        print("MPV side-data guard already present")
        return 0

    original = text
    text = replace_function(text, DESTRUCTOR_SIG, NEW_DESTRUCTOR)
    text = replace_function(text, NEW_REF_SIG, NEW_NEW_REF)

    if text == original:
        print("ERROR: no source changes were made", file=sys.stderr)
        return 1

    required = [
        "#define MP_IMAGE_MAX_FF_SIDE_DATA 64",
        "static bool mp_image_ff_side_data_valid",
        "bool copy_ff_side_data = mp_image_ff_side_data_valid(img);",
        "new->num_ff_side_data = 0;",
        "new->ff_side_data = NULL;",
    ]
    missing = [marker for marker in required if marker not in text]
    if missing:
        print(f"ERROR: patch verification failed; missing: {missing}", file=sys.stderr)
        return 1

    path.write_text(text, encoding="utf-8", newline="\n")
    print("Applied twiNX MPV hardware-frame side-data guard")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
