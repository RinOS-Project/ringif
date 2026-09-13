/*
 * RinOS GIF Decoder ✿
 * 軽量GIFデコーダー
 * GIF87a/GIF89a 対応
 */

#ifndef RINGIF_H
#define RINGIF_H

#include <stdint.h>
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════════
 * 定数
 * ═══════════════════════════════════════════════════════════════*/

#define RGIF_OK             0
#define RGIF_ERROR         -1
#define RGIF_DATA_ERROR    -2
#define RGIF_UNSUPPORTED   -3

#define RGIF_MAX_CODES     4096
#define RGIF_MAX_STACK     4096
#define RGIF_MAX_INPUT_BYTES (64u * 1024u * 1024u)
#define RGIF_MAX_DIMENSION 8192
#define RGIF_MAX_PIXELS (4096u * 4096u)
#define RGIF_MAX_FRAMES 1024

/* ═══════════════════════════════════════════════════════════════
 * 構造体
 * ═══════════════════════════════════════════════════════════════*/

#pragma pack(push, 1)
typedef struct {
    char     signature[3];   /* "GIF" */
    char     version[3];     /* "87a" or "89a" */
    uint16_t width;
    uint16_t height;
    uint8_t  packed;         /* Global Color Table Flag, etc */
    uint8_t  bg_color;
    uint8_t  aspect_ratio;
} RGifHeader;

typedef struct {
    uint16_t left;
    uint16_t top;
    uint16_t width;
    uint16_t height;
    uint8_t  packed;         /* Local Color Table Flag, etc */
} RGifImageDesc;
#pragma pack(pop)

typedef struct {
    uint8_t r, g, b;
} RGifColor;

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t pos;
    
    int width, height;
    int bg_color;
    
    /* グローバルカラーテーブル */
    RGifColor gct[256];
    int gct_size;
    
    /* LZWデコード用 */
    uint16_t prefix[RGIF_MAX_CODES];
    uint8_t  suffix[RGIF_MAX_CODES];
    uint8_t  stack[RGIF_MAX_STACK];
    
    /* フレーム情報 */
    int frame_count;
    int transparent_index;
    int disposal_method;
    int delay_time;
} RGifDecoder;

/* ═══════════════════════════════════════════════════════════════
 * ユーティリティ
 * ═══════════════════════════════════════════════════════════════*/

static inline uint8_t rgif_read8(RGifDecoder* d) {
    if (d->pos >= d->size) return 0;
    return d->data[d->pos++];
}

static inline uint16_t rgif_read16(RGifDecoder* d) {
    uint16_t lo = rgif_read8(d);
    uint16_t hi = rgif_read8(d);
    return lo | (hi << 8);
}

static inline void rgif_skip(RGifDecoder* d, size_t n) {
    d->pos += n;
    if (d->pos > d->size) d->pos = d->size;
}

/* ═══════════════════════════════════════════════════════════════
 * LZWデコード
 * ═══════════════════════════════════════════════════════════════*/

typedef struct {
    const uint8_t* data;
    size_t pos;
    size_t size;
    
    uint32_t bit_buf;
    int bit_count;
    
    /* サブブロック */
    uint8_t block[256];
    int block_size;
    int block_pos;
} RGifBitStream;

static inline int rgif_bs_next_byte(RGifBitStream* bs) {
    if (bs->block_pos >= bs->block_size) {
        /* 次のサブブロックを読む */
        if (bs->pos >= bs->size) return -1;
        bs->block_size = bs->data[bs->pos++];
        if (bs->block_size == 0) return -1;  /* ブロック終端 */

        if ((size_t)bs->block_size > bs->size - bs->pos) return -1;
        for (int i = 0; i < bs->block_size; i++) {
            bs->block[i] = bs->data[bs->pos++];
        }
        bs->block_pos = 0;
    }
    return bs->block[bs->block_pos++];
}

static inline int rgif_bs_get_bits(RGifBitStream* bs, int n) {
    if (!bs || n <= 0 || n > 12) return -1;
    while (bs->bit_count < n) {
        int b = rgif_bs_next_byte(bs);
        if (b < 0) return -1;
        bs->bit_buf |= (uint32_t)b << bs->bit_count;
        bs->bit_count += 8;
    }
    
    int val = bs->bit_buf & ((1 << n) - 1);
    bs->bit_buf >>= n;
    bs->bit_count -= n;
    return val;
}

static inline int rgif_lzw_decode(RGifDecoder* d, size_t data_start, 
                                   uint8_t* output, int output_size) {
    if (!d || !output || output_size <= 0 || data_start >= d->size) return -1;
    int min_code_size = d->data[data_start];
    if (min_code_size < 2 || min_code_size > 8) return -1;
    
#ifdef __cplusplus
    RGifBitStream bs = {};
#else
    RGifBitStream bs = {0};
#endif
    bs.data = d->data;
    bs.pos = data_start + 1;
    bs.size = d->size;
    bs.block_size = 0;
    bs.block_pos = 0;
    bs.bit_buf = 0;
    bs.bit_count = 0;
    
    int clear_code = 1 << min_code_size;
    int end_code = clear_code + 1;
    int next_code = end_code + 1;
    int code_size = min_code_size + 1;
    int code_mask = (1 << code_size) - 1;
    
    /* テーブル初期化 */
    for (int i = 0; i < clear_code; i++) {
        d->prefix[i] = RGIF_MAX_CODES;  /* 無効 */
        d->suffix[i] = i;
    }
    
    int out_pos = 0;
    int prev_code = -1;
    int first_char = 0;
    int saw_end_code = 0;
    
    while (!saw_end_code) {
        int code = rgif_bs_get_bits(&bs, code_size);
        if (code < 0) break;
        
        if (code == clear_code) {
            /* テーブルリセット */
            code_size = min_code_size + 1;
            code_mask = (1 << code_size) - 1;
            next_code = end_code + 1;
            prev_code = -1;
            continue;
        }
        
        if (code == end_code) {
            saw_end_code = 1;
            break;
        }

        if (code < 0 || code >= RGIF_MAX_CODES) return -1;
        
        int cur_code = code;
        int stack_pos = 0;
        
        if (code >= next_code) {
            /* 特殊ケース: コードがまだ定義されていない */
            if (code != next_code || prev_code < 0) return -1;
            d->stack[stack_pos++] = first_char;
            cur_code = prev_code;
        }
        
        /* コードを展開してスタックに積む */
        while (cur_code >= clear_code && stack_pos < RGIF_MAX_STACK) {
            if (cur_code < 0 || cur_code >= next_code ||
                cur_code >= RGIF_MAX_CODES) return -1;
            d->stack[stack_pos++] = d->suffix[cur_code];
            cur_code = d->prefix[cur_code];
        }
        if (cur_code < 0 || cur_code >= clear_code ||
            stack_pos >= RGIF_MAX_STACK) return -1;
        d->stack[stack_pos++] = d->suffix[cur_code];
        first_char = d->suffix[cur_code];
        
        /* スタックから出力（逆順） */
        while (stack_pos > 0 && out_pos < output_size) {
            output[out_pos++] = d->stack[--stack_pos];
        }
        
        /* 新しいコードをテーブルに追加 */
        if (prev_code >= 0 && next_code < RGIF_MAX_CODES) {
            d->prefix[next_code] = prev_code;
            d->suffix[next_code] = first_char;
            next_code++;
            
            if (next_code > code_mask && code_size < 12) {
                code_size++;
                code_mask = (1 << code_size) - 1;
            }
        }
        
        prev_code = code;
    }
    
    /* A complete image must carry an explicit end code.  Reaching the
     * caller-provided output bound is not proof that the compressed stream
     * terminated; accepting it would publish truncated pixels as a frame. */
    if (!saw_end_code || out_pos != output_size) return -1;

    /* 残りのサブブロックをスキップ */
    while (bs.pos < bs.size) {
        int block_size = bs.data[bs.pos++];
        if (block_size == 0) break;
        if ((size_t)block_size > bs.size - bs.pos) return -1;
        bs.pos += block_size;
    }
    
    d->pos = bs.pos;
    return out_pos;
}

/* ═══════════════════════════════════════════════════════════════
 * 公開API
 * ═══════════════════════════════════════════════════════════════*/

/*
 * GIF画像情報取得
 */
static inline int rgif_get_info(const uint8_t* data, size_t size,
                                 int* width, int* height, int* frame_count) {
    size_t pos;
    int logical_width;
    int logical_height;
    int gct_size = 0;
    int frames = 0;
    int saw_trailer = 0;
    uint8_t packed;
    if (!data || size < 13u || size > RGIF_MAX_INPUT_BYTES || !width ||
        !height)
        return RGIF_ERROR;

    if (data[0] != 'G' || data[1] != 'I' || data[2] != 'F' ||
        data[3] != '8' || (data[4] != '7' && data[4] != '9') ||
        data[5] != 'a') {
        return RGIF_DATA_ERROR;
    }
    logical_width = (int)data[6] | ((int)data[7] << 8);
    logical_height = (int)data[8] | ((int)data[9] << 8);
    if (logical_width <= 0 || logical_height <= 0 ||
        logical_width > RGIF_MAX_DIMENSION ||
        logical_height > RGIF_MAX_DIMENSION ||
        (size_t)logical_width * (size_t)logical_height > RGIF_MAX_PIXELS)
        return RGIF_UNSUPPORTED;
    packed = data[10];
    if ((packed & 0x08u) != 0u) return RGIF_DATA_ERROR;
    pos = 13u;
    if ((packed & 0x80u) != 0u) {
        gct_size = 1 << ((packed & 0x07u) + 1u);
        if ((size_t)gct_size * 3u > size - pos) return RGIF_DATA_ERROR;
        pos += (size_t)gct_size * 3u;
        if (data[11] >= (uint8_t)gct_size) return RGIF_DATA_ERROR;
    }
    while (pos < size) {
        uint8_t block = data[pos++];
        if (block == 0x2cu) {
            uint16_t left;
            uint16_t top;
            uint16_t frame_width;
            uint16_t frame_height;
            uint8_t image_packed;
            int lct_size = 0;
            if (frames >= RGIF_MAX_FRAMES || size - pos < 9u)
                return RGIF_DATA_ERROR;
            left = (uint16_t)data[pos] | ((uint16_t)data[pos + 1u] << 8);
            top = (uint16_t)data[pos + 2u] |
                  ((uint16_t)data[pos + 3u] << 8);
            frame_width = (uint16_t)data[pos + 4u] |
                          ((uint16_t)data[pos + 5u] << 8);
            frame_height = (uint16_t)data[pos + 6u] |
                           ((uint16_t)data[pos + 7u] << 8);
            image_packed = data[pos + 8u];
            pos += 9u;
            if (frame_width == 0u || frame_height == 0u ||
                left > (uint16_t)logical_width ||
                top > (uint16_t)logical_height ||
                frame_width > (uint16_t)(logical_width - left) ||
                frame_height > (uint16_t)(logical_height - top) ||
                (image_packed & 0x18u) != 0u)
                return RGIF_DATA_ERROR;
            if ((image_packed & 0x80u) != 0u) {
                lct_size = 1 << ((image_packed & 0x07u) + 1u);
                if ((size_t)lct_size * 3u > size - pos)
                    return RGIF_DATA_ERROR;
                pos += (size_t)lct_size * 3u;
            }
            if (pos >= size || data[pos++] < 2u || data[pos - 1u] > 8u)
                return RGIF_DATA_ERROR;
            for (;;) {
                size_t block_size;
                if (pos >= size) return RGIF_DATA_ERROR;
                block_size = data[pos++];
                if (block_size == 0u) break;
                if (block_size > size - pos) return RGIF_DATA_ERROR;
                pos += block_size;
            }
            ++frames;
        } else if (block == 0x21u) {
            if (pos >= size) return RGIF_DATA_ERROR;
            ++pos; /* extension label */
            for (;;) {
                size_t block_size;
                if (pos >= size) return RGIF_DATA_ERROR;
                block_size = data[pos++];
                if (block_size == 0u) break;
                if (block_size > size - pos) return RGIF_DATA_ERROR;
                pos += block_size;
            }
        } else if (block == 0x3bu) {
            saw_trailer = 1;
            if (pos != size) return RGIF_DATA_ERROR;
            break;
        } else {
            return RGIF_DATA_ERROR;
        }
    }
    if (!saw_trailer || frames == 0) return RGIF_DATA_ERROR;
    *width = logical_width;
    *height = logical_height;
    if (frame_count) *frame_count = frames;
    return RGIF_OK;
}

/*
 * GIFデコード（最初のフレームのみ）
 */
static inline int rgif_decode_with_scratch(
    const uint8_t* data, size_t size,
    uint32_t* pixels, int max_width, int max_height,
    uint8_t* index_buf, size_t index_capacity,
    RGifDecoder* decoder) {
    uint8_t* decoder_bytes;
    size_t decoder_index;
    int info_width = 0;
    int info_height = 0;
    if (!data || size < 13u || size > RGIF_MAX_INPUT_BYTES || !pixels ||
        !index_buf || !decoder ||
        rgif_get_info(data, size, &info_width, &info_height, NULL) != RGIF_OK)
        return RGIF_ERROR;
    decoder_bytes = (uint8_t*)decoder;
    for (decoder_index = 0u; decoder_index < sizeof(*decoder);
         ++decoder_index)
        decoder_bytes[decoder_index] = 0u;

/* Keep the original, audited state accesses while moving their storage to
 * caller-owned scratch. The macro is confined to this function body. */
#define d (*decoder)
    d.data = data;
    d.size = size;
    d.transparent_index = -1;
    
    if (data[0] != 'G' || data[1] != 'I' || data[2] != 'F') {
        return RGIF_DATA_ERROR;
    }
    d.width = info_width;
    d.height = info_height;
    d.bg_color = data[11];

    size_t screen_pixels = (size_t)d.width * (size_t)d.height;
    if (d.width <= 0 || d.height <= 0 || d.width > max_width ||
        d.height > max_height || screen_pixels > index_capacity ||
        screen_pixels > (size_t)INT32_MAX) return RGIF_ERROR;
    
    d.pos = 13;
    
    /* グローバルカラーテーブル読み込み */
    if (data[10] & 0x80u) {
        d.gct_size = 1 << ((data[10] & 0x07u) + 1u);
        if ((size_t)d.gct_size * 3u > d.size - d.pos) return RGIF_DATA_ERROR;
        for (int i = 0; i < d.gct_size; i++) {
            d.gct[i].r = d.data[d.pos++];
            d.gct[i].g = d.data[d.pos++];
            d.gct[i].b = d.data[d.pos++];
        }
    }
    
    /* 背景色で初期化 */
    RGifColor* ct = d.gct;
    RGifColor background = {0u, 0u, 0u};
    if (d.gct_size > 0) background = d.gct[d.bg_color];
    
    /* ブロック解析 */
    while (d.pos < d.size) {
        uint8_t block = rgif_read8(&d);
        
        if (block == 0x2C) {
            /* Image Descriptor */
            if (d.size - d.pos < 9u) return RGIF_DATA_ERROR;
            RGifImageDesc img;
            img.left = rgif_read16(&d);
            img.top = rgif_read16(&d);
            img.width = rgif_read16(&d);
            img.height = rgif_read16(&d);
            img.packed = rgif_read8(&d);

            if (img.width == 0u || img.height == 0u ||
                img.left > (uint16_t)d.width || img.top > (uint16_t)d.height ||
                img.width > (uint16_t)(d.width - img.left) ||
                img.height > (uint16_t)(d.height - img.top) ||
                (img.packed & 0x18u) != 0u)
                return RGIF_DATA_ERROR;
            
            /* ローカルカラーテーブル */
            RGifColor lct[256];
            RGifColor* color_table = ct;
            int color_table_size = d.gct_size;
            
            if (img.packed & 0x80) {
                int lct_size = 1 << ((img.packed & 0x07) + 1);
                if ((size_t)lct_size * 3u > d.size - d.pos) {
                    return RGIF_DATA_ERROR;
                }
                for (int i = 0; i < lct_size; i++) {
                    lct[i].r = d.data[d.pos++];
                    lct[i].g = d.data[d.pos++];
                    lct[i].b = d.data[d.pos++];
                }
                color_table = lct;
                color_table_size = lct_size;
            }
            
            /* LZW indices live only for the duration of the decode. */
            size_t pixel_count = (size_t)img.width * (size_t)img.height;
            if (img.width == 0u || img.height == 0u ||
                pixel_count > index_capacity || pixel_count > (size_t)INT32_MAX) {
                return RGIF_ERROR;
            }
            int max_decode = (int)pixel_count;
            int decoded = rgif_lzw_decode(&d, d.pos, index_buf, max_decode);
            if (decoded != max_decode) return RGIF_DATA_ERROR;

            /* Validate every palette index before touching the caller's
             * pixels.  A malformed frame must not leave a half-rendered
             * bitmap behind on the failure path. */
            for (int i = 0; i < decoded; ++i) {
                int idx = index_buf[i];
                if (idx != d.transparent_index &&
                    (idx < 0 || idx >= color_table_size))
                    return RGIF_DATA_ERROR;
            }

            for (int i = 0; i < d.width * d.height; i++) {
                pixels[i] = (0xFFu << 24) | ((uint32_t)background.r << 16) |
                            ((uint32_t)background.g << 8) | background.b;
            }

            if (decoded > 0) {
                /* インデックスをRGBに変換 */
                int interlace = (img.packed & 0x40) != 0;
                int pass = 0;
                int y = 0;
                static const int interlace_start[4] = {0, 4, 2, 1};
                static const int interlace_step[4] = {8, 8, 4, 2};
                
                if (interlace) y = interlace_start[0];
                
                for (int i = 0; i < decoded; ) {
                    int x = i % img.width;
                    
                    if (x == 0 && i > 0) {
                        if (interlace) {
                            y += interlace_step[pass];
                            while (y >= img.height && pass < 3) {
                                pass++;
                                y = interlace_start[pass];
                            }
                        } else {
                            y++;
                        }
                    }
                    
                    if (y < img.height) {
                        int px = img.left + x;
                        int py = img.top + y;
                        
                        if (px >= 0 && py >= 0 && px < d.width && py < d.height) {
                            int idx = index_buf[i];
                            if (idx != d.transparent_index) {
                                RGifColor c = color_table[idx];
                                pixels[py * d.width + px] = (0xFF << 24) | 
                                    (c.r << 16) | (c.g << 8) | c.b;
                            }
                        }
                    }
                    i++;
                }
            }
            
            /* 最初のフレームのみデコード */
            break;
            
        } else if (block == 0x21) {
            /* Extension */
            if (d.pos >= d.size) return RGIF_DATA_ERROR;
            uint8_t ext_type = rgif_read8(&d);
            
            if (ext_type == 0xF9) {
                /* Graphic Control Extension */
                uint8_t block_size = rgif_read8(&d);
                if (block_size != 4u || (size_t)block_size >= d.size - d.pos) {
                    return RGIF_DATA_ERROR;
                }
                uint8_t packed = rgif_read8(&d);
                d.delay_time = rgif_read16(&d);
                d.transparent_index = rgif_read8(&d);
                d.disposal_method = (packed >> 2) & 0x07;

                if ((packed & 0xe0u) != 0u || d.disposal_method > 3)
                    return RGIF_DATA_ERROR;

                if (!(packed & 0x01)) {
                    d.transparent_index = -1;
                }
                if (rgif_read8(&d) != 0u) return RGIF_DATA_ERROR;
            } else {
                /* その他のエクステンションをスキップ */
                int terminated = 0;
                while (d.pos < d.size) {
                    int block_size = rgif_read8(&d);
                    if (block_size == 0) {
                        terminated = 1;
                        break;
                    }
                    if ((size_t)block_size > d.size - d.pos) return RGIF_DATA_ERROR;
                    rgif_skip(&d, block_size);
                }
                if (!terminated) return RGIF_DATA_ERROR;
            }
            
        } else if (block == 0x3B) {
            /* Trailer */
            break;
        }
    }
    
    return RGIF_OK;
}
#undef d

static inline int rgif_decode(const uint8_t* data, size_t size,
                               uint32_t* pixels, int max_width, int max_height,
                               uint8_t* index_buf, size_t index_capacity) {
#ifdef __cplusplus
    RGifDecoder decoder = {};
#else
    RGifDecoder decoder = {0};
#endif
    return rgif_decode_with_scratch(data, size, pixels, max_width, max_height,
                                    index_buf, index_capacity, &decoder);
}

#endif /* RINGIF_H */
