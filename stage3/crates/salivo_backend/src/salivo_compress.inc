/*
 * salivo_compress.inc - DEFLATE (RFC 1951), zlib (RFC 1950), and gzip (RFC 1952) codec.
 * Pure C99, thread-safe (no written globals), no external dependencies.
 */

#ifndef SALIVO_COMPRESS_INC
#define SALIVO_COMPRESS_INC

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* --- Lookup Tables & Constants --- */

static const uint32_t cz_crc32_table[256] = {
    0x00000000U, 0x77073096U, 0xEE0E612CU, 0x990951BAU,
    0x076DC419U, 0x706AF48FU, 0xE963A535U, 0x9E6495A3U,
    0x0EDB8832U, 0x79DCB8A4U, 0xE0D5E91EU, 0x97D2D988U,
    0x09B64C2BU, 0x7EB17CBDU, 0xE7B82D07U, 0x90BF1D91U,
    0x1DB71064U, 0x6AB020F2U, 0xF3B97148U, 0x84BE41DEU,
    0x1ADAD47DU, 0x6DDDE4EBU, 0xF4D4B551U, 0x83D385C7U,
    0x136C9856U, 0x646BA8C0U, 0xFD62F97AU, 0x8A65C9ECU,
    0x14015C4FU, 0x63066CD9U, 0xFA0F3D63U, 0x8D080DF5U,
    0x3B6E20C8U, 0x4C69105EU, 0xD56041E4U, 0xA2677172U,
    0x3C03E4D1U, 0x4B04D447U, 0xD20D85FDU, 0xA50AB56BU,
    0x35B5A8FAU, 0x42B2986CU, 0xDBBBC9D6U, 0xACBCF940U,
    0x32D86CE3U, 0x45DF5C75U, 0xDCD60DCFU, 0xABD13D59U,
    0x26D930ACU, 0x51DE003AU, 0xC8D75180U, 0xBFD06116U,
    0x21B4F4B5U, 0x56B3C423U, 0xCFBA9599U, 0xB8BDA50FU,
    0x2802B89EU, 0x5F058808U, 0xC60CD9B2U, 0xB10BE924U,
    0x2F6F7C87U, 0x58684C11U, 0xC1611DABU, 0xB6662D3DU,
    0x76DC4190U, 0x01DB7106U, 0x98D220BCU, 0xEFD5102AU,
    0x71B18589U, 0x06B6B51FU, 0x9FBFE4A5U, 0xE8B8D433U,
    0x7807C9A2U, 0x0F00F934U, 0x9609A88EU, 0xE10E9818U,
    0x7F6A0DBBU, 0x086D3D2DU, 0x91646C97U, 0xE6635C01U,
    0x6B6B51F4U, 0x1C6C6162U, 0x856530D8U, 0xF262004EU,
    0x6C0695EDU, 0x1B01A57BU, 0x8208F4C1U, 0xF50FC457U,
    0x65B0D9C6U, 0x12B7E950U, 0x8BBEB8EAU, 0xFCB9887CU,
    0x62DD1DDFU, 0x15DA2D49U, 0x8CD37CF3U, 0xFBD44C65U,
    0x4DB26158U, 0x3AB551CEU, 0xA3BC0074U, 0xD4BB30E2U,
    0x4ADFA541U, 0x3DD895D7U, 0xA4D1C46DU, 0xD3D6F4FBU,
    0x4369E96AU, 0x346ED9FCU, 0xAD678846U, 0xDA60B8D0U,
    0x44042D73U, 0x33031DE5U, 0xAA0A4C5FU, 0xDD0D7CC9U,
    0x5005713CU, 0x270241AAU, 0xBE0B1010U, 0xC90C2086U,
    0x5768B525U, 0x206F85B3U, 0xB966D409U, 0xCE61E49FU,
    0x5EDEF90EU, 0x29D9C998U, 0xB0D09822U, 0xC7D7A8B4U,
    0x59B33D17U, 0x2EB40D81U, 0xB7BD5C3BU, 0xC0BA6CADU,
    0xEDB88320U, 0x9ABFB3B6U, 0x03B6E20CU, 0x74B1D29AU,
    0xEAD54739U, 0x9DD277AFU, 0x04DB2615U, 0x73DC1683U,
    0xE3630B12U, 0x94643B84U, 0x0D6D6A3EU, 0x7A6A5AA8U,
    0xE40ECF0BU, 0x9309FF9DU, 0x0A00AE27U, 0x7D079EB1U,
    0xF00F9344U, 0x8708A3D2U, 0x1E01F268U, 0x6906C2FEU,
    0xF762575DU, 0x806567CBU, 0x196C3671U, 0x6E6B06E7U,
    0xFED41B76U, 0x89D32BE0U, 0x10DA7A5AU, 0x67DD4ACCU,
    0xF9B9DF6FU, 0x8EBEEFF9U, 0x17B7BE43U, 0x60B08ED5U,
    0xD6D6A3E8U, 0xA1D1937EU, 0x38D8C2C4U, 0x4FDFF252U,
    0xD1BB67F1U, 0xA6BC5767U, 0x3FB506DDU, 0x48B2364BU,
    0xD80D2BDAU, 0xAF0A1B4CU, 0x36034AF6U, 0x41047A60U,
    0xDF60EFC3U, 0xA867DF55U, 0x316E8EEFU, 0x4669BE79U,
    0xCB61B38CU, 0xBC66831AU, 0x256FD2A0U, 0x5268E236U,
    0xCC0C7795U, 0xBB0B4703U, 0x220216B9U, 0x5505262FU,
    0xC5BA3BBEU, 0xB2BD0B28U, 0x2BB45A92U, 0x5CB36A04U,
    0xC2D7FFA7U, 0xB5D0CF31U, 0x2CD99E8BU, 0x5BDEAE1DU,
    0x9B64C2B0U, 0xEC63F226U, 0x756AA39CU, 0x026D930AU,
    0x9C0906A9U, 0xEB0E363FU, 0x72076785U, 0x05005713U,
    0x95BF4A82U, 0xE2B87A14U, 0x7BB12BAEU, 0x0CB61B38U,
    0x92D28E9BU, 0xE5D5BE0DU, 0x7CDCEFB7U, 0x0BDBDF21U,
    0x86D3D2D4U, 0xF1D4E242U, 0x68DDB3F8U, 0x1FDA836EU,
    0x81BE16CDU, 0xF6B9265BU, 0x6FB077E1U, 0x18B74777U,
    0x88085AE6U, 0xFF0F6A70U, 0x66063BCAU, 0x11010B5CU,
    0x8F659EFFU, 0xF862AE69U, 0x616BFFD3U, 0x166CCF45U,
    0xA00AE278U, 0xD70DD2EEU, 0x4E048354U, 0x3903B3C2U,
    0xA7672661U, 0xD06016F7U, 0x4969474DU, 0x3E6E77DBU,
    0xAED16A4AU, 0xD9D65ADCU, 0x40DF0B66U, 0x37D83BF0U,
    0xA9BCAE53U, 0xDEBB9EC5U, 0x47B2CF7FU, 0x30B5FFE9U,
    0xBDBDF21CU, 0xCABAC28AU, 0x53B39330U, 0x24B4A3A6U,
    0xBAD03605U, 0xCDD70693U, 0x54DE5729U, 0x23D967BFU,
    0xB3667A2EU, 0xC4614AB8U, 0x5D681B02U, 0x2A6F2B94U,
    0xB40BBE37U, 0xC30C8EA1U, 0x5A05DF1BU, 0x2D02EF8DU
};

static const uint16_t cz_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};

static const uint8_t cz_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

static const uint16_t cz_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};

static const uint8_t cz_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static const uint8_t cz_cl_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* --- Checksum Implementations --- */

static uint32_t cz_crc32(const uint8_t *buf, size_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ cz_crc32_table[(crc ^ buf[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFFU;
}

static uint32_t cz_adler32(const uint8_t *buf, size_t len) {
    uint32_t s1 = 1;
    uint32_t s2 = 0;
    while (len > 0) {
        size_t block = len > 5552 ? 5552 : len;
        len -= block;
        while (block >= 16) {
            s1 += buf[0]; s2 += s1;
            s1 += buf[1]; s2 += s1;
            s1 += buf[2]; s2 += s1;
            s1 += buf[3]; s2 += s1;
            s1 += buf[4]; s2 += s1;
            s1 += buf[5]; s2 += s1;
            s1 += buf[6]; s2 += s1;
            s1 += buf[7]; s2 += s1;
            s1 += buf[8]; s2 += s1;
            s1 += buf[9]; s2 += s1;
            s1 += buf[10]; s2 += s1;
            s1 += buf[11]; s2 += s1;
            s1 += buf[12]; s2 += s1;
            s1 += buf[13]; s2 += s1;
            s1 += buf[14]; s2 += s1;
            s1 += buf[15]; s2 += s1;
            buf += 16;
            block -= 16;
        }
        while (block > 0) {
            s1 += *buf++;
            s2 += s1;
            block--;
        }
        s1 %= 65521U;
        s2 %= 65521U;
    }
    return (s2 << 16) | s1;
}

long long salivo_crc32(long long addr, long long len) {
    if (len < 0) return 0;
    if (addr == 0 && len > 0) return 0;
    return (long long)cz_crc32((const uint8_t *)(uintptr_t)addr, (size_t)len);
}

long long salivo_adler32(long long addr, long long len) {
    if (len < 0) return 1;
    if (addr == 0 && len > 0) return 1;
    return (long long)cz_adler32((const uint8_t *)(uintptr_t)addr, (size_t)len);
}

long long salivo_deflate_bound(long long in_len) {
    if (in_len < 0) return -1;
    /* Safe upper bound: stored-block encoding overhead + headers/trailers */
    return in_len + (in_len / 16384 + 1) * 5 + 64;
}

/* --- Bit Reader for INFLATE --- */

typedef struct {
    const uint8_t *in;
    size_t in_len;
    size_t in_pos;
    uint64_t bit_buf;
    int bit_cnt;
} cz_in_bits_t;

static inline void cz_fill_bits(cz_in_bits_t *b, int needed) {
    while (b->bit_cnt < needed && b->in_pos < b->in_len) {
        b->bit_buf |= ((uint64_t)b->in[b->in_pos++]) << b->bit_cnt;
        b->bit_cnt += 8;
    }
}

static inline void cz_drop_bits(cz_in_bits_t *b, int n) {
    b->bit_buf >>= n;
    b->bit_cnt -= n;
}

static inline uint32_t cz_read_bits(cz_in_bits_t *b, int n) {
    cz_fill_bits(b, n);
    uint32_t val = (uint32_t)(b->bit_buf & ((1ULL << n) - 1));
    b->bit_buf >>= n;
    b->bit_cnt -= n;
    return val;
}

static inline void cz_byte_align(cz_in_bits_t *b) {
    int rem = b->bit_cnt & 7;
    b->bit_buf >>= rem;
    b->bit_cnt -= rem;
    /* Rewind unconsumed whole bytes held in bit_buf */
    b->in_pos -= (size_t)(b->bit_cnt / 8);
    b->bit_buf = 0;
    b->bit_cnt = 0;
}

/* --- Huffman Table Construction for Decoder --- */

static int cz_build_huffman_table(const uint8_t *lens, int num_symbols, uint16_t *table, int table_size) {
    int count[16] = {0};
    for (int i = 0; i < num_symbols; i++) {
        if (lens[i] > 15) return -1;
        count[lens[i]]++;
    }
    count[0] = 0;

    int code = 0;
    int next_code[16] = {0};
    for (int bits = 1; bits <= 15; bits++) {
        code = (code + count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    if (code + count[15] > (1 << 15)) {
        return -1; /* Over-subscribed tree */
    }

    memset(table, 0, (size_t)table_size * sizeof(uint16_t));

    for (int i = 0; i < num_symbols; i++) {
        int len = lens[i];
        if (len == 0) continue;
        int c = next_code[len]++;
        uint32_t rev = 0;
        for (int b = 0; b < len; b++) {
            rev |= (((uint32_t)((c >> b) & 1)) << (len - 1 - b));
        }
        uint16_t entry = (uint16_t)((i << 4) | len);
        for (uint32_t fill = rev; fill < (uint32_t)table_size; fill += (1U << len)) {
            table[fill] = entry;
        }
    }
    return 0;
}

static int cz_build_cl_table(const uint8_t *lens, int num_symbols, uint8_t *table) {
    int count[8] = {0};
    for (int i = 0; i < num_symbols; i++) {
        if (lens[i] > 7) return -1;
        count[lens[i]]++;
    }
    count[0] = 0;

    int code = 0;
    int next_code[8] = {0};
    for (int bits = 1; bits <= 7; bits++) {
        code = (code + count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    if (code + count[7] > (1 << 7)) {
        return -1;
    }

    memset(table, 0, 128);

    for (int i = 0; i < num_symbols; i++) {
        int len = lens[i];
        if (len == 0) continue;
        int c = next_code[len]++;
        uint32_t rev = 0;
        for (int b = 0; b < len; b++) {
            rev |= (((uint32_t)((c >> b) & 1)) << (len - 1 - b));
        }
        uint8_t entry = (uint8_t)((i << 3) | len);
        for (uint32_t fill = rev; fill < 128; fill += (1U << len)) {
            table[fill] = entry;
        }
    }
    return 0;
}

/* --- INFLATE --- */

long long salivo_inflate(long long in_addr, long long in_len, long long out_addr, long long out_cap, long long format) {
    if (in_len < 0 || out_cap < 0) return -1;
    if (in_addr == 0 && in_len > 0) return -1;
    if (out_addr == 0 && out_cap > 0) return -1;

    const uint8_t *in = (const uint8_t *)(uintptr_t)in_addr;
    uint8_t *out = (uint8_t *)(uintptr_t)out_addr;

    /* Auto-detect format */
    if (format == 3) {
        if (in_len >= 10 && in[0] == 0x1f && in[1] == 0x8b) {
            format = 2; /* gzip */
        } else if (in_len >= 6 && (((in[0] << 8) | in[1]) % 31 == 0) &&
                   ((in[0] & 0x0F) == 8) && ((in[0] >> 4) <= 7)) {
            format = 1; /* zlib */
        } else {
            format = 0; /* raw */
        }
    }

    cz_in_bits_t b;
    b.in = in;
    b.in_len = (size_t)in_len;
    b.in_pos = 0;
    b.bit_buf = 0;
    b.bit_cnt = 0;

    size_t out_pos = 0;

    /* Parse Format Header */
    if (format == 1) {
        /* zlib header: CMF, FLG (2 bytes) */
        if (b.in_len < 6) return -1;
        uint8_t cmf = in[0];
        uint8_t flg = in[1];
        if (((cmf << 8) | flg) % 31 != 0) return -1;
        if ((cmf & 0x0F) != 8) return -1;  /* only deflate */
        if ((cmf >> 4) > 7) return -1;     /* window size up to 32K */
        if (flg & 0x20) return -1;         /* FDICT preset dict not supported */
        b.in_pos = 2;
    } else if (format == 2) {
        /* gzip header (RFC 1952) */
        if (b.in_len < 18) return -1;
        if (in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) return -1;
        uint8_t flg = in[3];
        b.in_pos = 10;
        if (flg & 4) { /* FEXTRA */
            if (b.in_pos + 2 > b.in_len) return -1;
            uint16_t xlen = (uint16_t)in[b.in_pos] | ((uint16_t)in[b.in_pos + 1] << 8);
            b.in_pos += 2 + xlen;
            if (b.in_pos > b.in_len) return -1;
        }
        if (flg & 8) { /* FNAME */
            while (b.in_pos < b.in_len && in[b.in_pos] != 0) b.in_pos++;
            if (b.in_pos >= b.in_len) return -1;
            b.in_pos++;
        }
        if (flg & 16) { /* FCOMMENT */
            while (b.in_pos < b.in_len && in[b.in_pos] != 0) b.in_pos++;
            if (b.in_pos >= b.in_len) return -1;
            b.in_pos++;
        }
        if (flg & 2) { /* FHCRC */
            b.in_pos += 2;
            if (b.in_pos > b.in_len) return -1;
        }
    } else if (format != 0) {
        return -1;
    }

    /* Allocate Huffman lookup tables (32768 entries each = 64KB * 2) */
    uint16_t *lit_table = (uint16_t *)malloc(32768 * sizeof(uint16_t));
    uint16_t *dist_table = (uint16_t *)malloc(32768 * sizeof(uint16_t));
    if (!lit_table || !dist_table) {
        if (lit_table) free(lit_table);
        if (dist_table) free(dist_table);
        return -1;
    }

    int bfinal = 0;
    int err = 0;

    while (!bfinal) {
        cz_fill_bits(&b, 3);
        if (b.bit_cnt < 3 && b.in_pos >= b.in_len) {
            err = -1;
            break;
        }
        bfinal = (int)cz_read_bits(&b, 1);
        int btype = (int)cz_read_bits(&b, 2);

        if (btype == 0) {
            /* Stored block */
            cz_byte_align(&b);
            if (b.in_pos + 4 > b.in_len) { err = -1; break; }
            uint16_t blen = (uint16_t)b.in[b.in_pos] | ((uint16_t)b.in[b.in_pos + 1] << 8);
            uint16_t bnlen = (uint16_t)b.in[b.in_pos + 2] | ((uint16_t)b.in[b.in_pos + 3] << 8);
            b.in_pos += 4;
            if (blen != (uint16_t)(~bnlen)) { err = -1; break; }
            if (b.in_pos + blen > b.in_len) { err = -1; break; }
            if (out_pos + blen > (size_t)out_cap) { err = -2; break; }
            if (blen > 0) {
                memcpy(out + out_pos, b.in + b.in_pos, blen);
                out_pos += blen;
                b.in_pos += blen;
            }
        } else if (btype == 1 || btype == 2) {
            /* Huffman block */
            if (btype == 1) {
                /* Fixed Huffman tree */
                uint8_t fixed_lit_lens[288];
                for (int i = 0; i <= 143; i++) fixed_lit_lens[i] = 8;
                for (int i = 144; i <= 255; i++) fixed_lit_lens[i] = 9;
                for (int i = 256; i <= 279; i++) fixed_lit_lens[i] = 7;
                for (int i = 280; i <= 287; i++) fixed_lit_lens[i] = 8;

                uint8_t fixed_dist_lens[32];
                for (int i = 0; i < 32; i++) fixed_dist_lens[i] = 5;

                if (cz_build_huffman_table(fixed_lit_lens, 288, lit_table, 32768) < 0 ||
                    cz_build_huffman_table(fixed_dist_lens, 32, dist_table, 32768) < 0) {
                    err = -1;
                    break;
                }
            } else {
                /* Dynamic Huffman tree */
                cz_fill_bits(&b, 14);
                if (b.bit_cnt < 14 && b.in_pos >= b.in_len) { err = -1; break; }
                int hlit = (int)cz_read_bits(&b, 5) + 257;
                int hdist = (int)cz_read_bits(&b, 5) + 1;
                int hclen = (int)cz_read_bits(&b, 4) + 4;

                if (hlit > 286 || hdist > 32) { err = -1; break; }

                uint8_t cl_lens[19] = {0};
                for (int i = 0; i < hclen; i++) {
                    cz_fill_bits(&b, 3);
                    if (b.bit_cnt < 3 && b.in_pos >= b.in_len) { err = -1; break; }
                    cl_lens[cz_cl_order[i]] = (uint8_t)cz_read_bits(&b, 3);
                }
                if (err < 0) break;

                uint8_t cl_table[128];
                if (cz_build_cl_table(cl_lens, 19, cl_table) < 0) { err = -1; break; }

                uint8_t code_lens[288 + 32] = {0};
                int total_codes = hlit + hdist;
                int idx = 0;

                while (idx < total_codes) {
                    cz_fill_bits(&b, 7);
                    uint8_t cle = cl_table[b.bit_buf & 0x7F];
                    if (cle == 0) { err = -1; break; }
                    int cl_len = cle & 7;
                    if (b.bit_cnt < cl_len && b.in_pos >= b.in_len) { err = -1; break; }
                    cz_drop_bits(&b, cl_len);
                    int sym = cle >> 3;

                    if (sym < 16) {
                        code_lens[idx++] = (uint8_t)sym;
                    } else if (sym == 16) {
                        if (idx == 0) { err = -1; break; }
                        uint8_t prev_len = code_lens[idx - 1];
                        cz_fill_bits(&b, 2);
                        if (b.bit_cnt < 2 && b.in_pos >= b.in_len) { err = -1; break; }
                        int repeat = 3 + (int)cz_read_bits(&b, 2);
                        if (idx + repeat > total_codes) { err = -1; break; }
                        while (repeat-- > 0) code_lens[idx++] = prev_len;
                    } else if (sym == 17) {
                        cz_fill_bits(&b, 3);
                        if (b.bit_cnt < 3 && b.in_pos >= b.in_len) { err = -1; break; }
                        int repeat = 3 + (int)cz_read_bits(&b, 3);
                        if (idx + repeat > total_codes) { err = -1; break; }
                        while (repeat-- > 0) code_lens[idx++] = 0;
                    } else if (sym == 18) {
                        cz_fill_bits(&b, 7);
                        if (b.bit_cnt < 7 && b.in_pos >= b.in_len) { err = -1; break; }
                        int repeat = 11 + (int)cz_read_bits(&b, 7);
                        if (idx + repeat > total_codes) { err = -1; break; }
                        while (repeat-- > 0) code_lens[idx++] = 0;
                    } else {
                        err = -1;
                        break;
                    }
                }
                if (err < 0) break;

                /* EOB (256) must be present */
                if (code_lens[256] == 0) { err = -1; break; }

                if (cz_build_huffman_table(code_lens, hlit, lit_table, 32768) < 0 ||
                    cz_build_huffman_table(code_lens + hlit, hdist, dist_table, 32768) < 0) {
                    err = -1;
                    break;
                }
            }

            /* Decode Huffman Stream */
            for (;;) {
                cz_fill_bits(&b, 15);
                uint16_t e = lit_table[b.bit_buf & 0x7FFF];
                if (e == 0) { err = -1; break; }
                int len = e & 0x0F;
                if (b.bit_cnt < len && b.in_pos >= b.in_len) { err = -1; break; }
                cz_drop_bits(&b, len);
                uint32_t sym = e >> 4;

                if (sym < 256) {
                    if (out_pos >= (size_t)out_cap) { err = -2; break; }
                    out[out_pos++] = (uint8_t)sym;
                } else if (sym == 256) {
                    /* End of block */
                    break;
                } else if (sym <= 285) {
                    int len_idx = (int)sym - 257;
                    int match_len = cz_len_base[len_idx];
                    int extra_l = cz_len_extra[len_idx];
                    if (extra_l > 0) {
                        cz_fill_bits(&b, extra_l);
                        if (b.bit_cnt < extra_l && b.in_pos >= b.in_len) { err = -1; break; }
                        match_len += (int)cz_read_bits(&b, extra_l);
                    }

                    /* Decode distance */
                    cz_fill_bits(&b, 15);
                    uint16_t de = dist_table[b.bit_buf & 0x7FFF];
                    if (de == 0) { err = -1; break; }
                    int dlen = de & 0x0F;
                    if (b.bit_cnt < dlen && b.in_pos >= b.in_len) { err = -1; break; }
                    cz_drop_bits(&b, dlen);
                    uint32_t dist_sym = de >> 4;
                    if (dist_sym >= 30) { err = -1; break; }

                    int match_dist = cz_dist_base[dist_sym];
                    int extra_d = cz_dist_extra[dist_sym];
                    if (extra_d > 0) {
                        cz_fill_bits(&b, extra_d);
                        if (b.bit_cnt < extra_d && b.in_pos >= b.in_len) { err = -1; break; }
                        match_dist += (int)cz_read_bits(&b, extra_d);
                    }

                    if (match_dist > (int)out_pos) { err = -1; break; }
                    if (out_pos + (size_t)match_len > (size_t)out_cap) { err = -2; break; }

                    for (int k = 0; k < match_len; k++) {
                        out[out_pos] = out[out_pos - match_dist];
                        out_pos++;
                    }
                } else {
                    err = -1;
                    break;
                }
            }
            if (err < 0) break;
        } else {
            err = -1; /* Reserved block type */
            break;
        }
    }

    free(lit_table);
    free(dist_table);

    if (err < 0) return err;

    /* Verify Trailers */
    if (format == 1) {
        cz_byte_align(&b);
        if (b.in_pos + 4 > b.in_len) return -1;
        uint32_t expected_adler = ((uint32_t)b.in[b.in_pos] << 24) |
                                  ((uint32_t)b.in[b.in_pos + 1] << 16) |
                                  ((uint32_t)b.in[b.in_pos + 2] << 8) |
                                  ((uint32_t)b.in[b.in_pos + 3]);
        b.in_pos += 4;
        uint32_t actual_adler = cz_adler32(out, out_pos);
        if (expected_adler != actual_adler) return -1;
    } else if (format == 2) {
        cz_byte_align(&b);
        if (b.in_pos + 8 > b.in_len) return -1;
        uint32_t expected_crc = (uint32_t)b.in[b.in_pos] |
                                ((uint32_t)b.in[b.in_pos + 1] << 8) |
                                ((uint32_t)b.in[b.in_pos + 2] << 16) |
                                ((uint32_t)b.in[b.in_pos + 3] << 24);
        uint32_t expected_isize = (uint32_t)b.in[b.in_pos + 4] |
                                  ((uint32_t)b.in[b.in_pos + 5] << 8) |
                                  ((uint32_t)b.in[b.in_pos + 6] << 16) |
                                  ((uint32_t)b.in[b.in_pos + 7] << 24);
        b.in_pos += 8;
        uint32_t actual_crc = cz_crc32(out, out_pos);
        if (expected_crc != actual_crc) return -1;
        if (expected_isize != (uint32_t)(out_pos & 0xFFFFFFFFU)) return -1;
    }

    return (long long)out_pos;
}

/* --- Bit Writer for DEFLATE --- */

typedef struct {
    uint8_t *out;
    size_t out_cap;
    size_t out_pos;
    uint64_t bit_buf;
    int bit_cnt;
    int overflow;
} cz_out_bits_t;

static inline void cz_put_bits(cz_out_bits_t *b, uint32_t val, int n) {
    b->bit_buf |= ((uint64_t)val) << b->bit_cnt;
    b->bit_cnt += n;
    while (b->bit_cnt >= 8) {
        if (b->out_pos < b->out_cap) {
            b->out[b->out_pos++] = (uint8_t)(b->bit_buf & 0xFF);
        } else {
            b->overflow = 1;
        }
        b->bit_buf >>= 8;
        b->bit_cnt -= 8;
    }
}

static inline void cz_put_flush(cz_out_bits_t *b) {
    if (b->bit_cnt > 0) {
        if (b->out_pos < b->out_cap) {
            b->out[b->out_pos++] = (uint8_t)(b->bit_buf & 0xFF);
        } else {
            b->overflow = 1;
        }
        b->bit_buf = 0;
        b->bit_cnt = 0;
    }
}

/* --- Fixed Huffman Emission Helpers --- */

static inline void cz_get_fixed_lit_code(int sym, uint32_t *code, int *len) {
    uint32_t c;
    int l;
    if (sym <= 143) {
        c = (uint32_t)(0x30 + sym);
        l = 8;
    } else if (sym <= 255) {
        c = (uint32_t)(0x190 + (sym - 144));
        l = 9;
    } else if (sym <= 279) {
        c = (uint32_t)(sym - 256);
        l = 7;
    } else {
        c = (uint32_t)(0xC0 + (sym - 280));
        l = 8;
    }
    uint32_t rev = 0;
    for (int i = 0; i < l; i++) {
        rev |= (((c >> i) & 1) << (l - 1 - i));
    }
    *code = rev;
    *len = l;
}

static inline void cz_get_fixed_dist_code(int dist_sym, uint32_t *code, int *len) {
    uint32_t rev = 0;
    for (int i = 0; i < 5; i++) {
        rev |= ((((uint32_t)dist_sym >> i) & 1) << (4 - i));
    }
    *code = rev;
    *len = 5;
}

static inline int cz_len_to_sym(int len) {
    int lo = 0, hi = 28;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (cz_len_base[mid] <= len) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo + 257;
}

static inline int cz_dist_to_sym(int dist) {
    int lo = 0, hi = 29;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (cz_dist_base[mid] <= dist) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

static inline void cz_emit_fixed_lit(cz_out_bits_t *b, int sym) {
    uint32_t code;
    int len;
    cz_get_fixed_lit_code(sym, &code, &len);
    cz_put_bits(b, code, len);
}

static inline void cz_emit_fixed_match(cz_out_bits_t *b, int match_len, int match_dist) {
    int len_sym = cz_len_to_sym(match_len);
    int len_extra_bits = cz_len_extra[len_sym - 257];
    int len_extra_val = match_len - cz_len_base[len_sym - 257];

    uint32_t code;
    int len;
    cz_get_fixed_lit_code(len_sym, &code, &len);
    cz_put_bits(b, code, len);
    if (len_extra_bits > 0) {
        cz_put_bits(b, (uint32_t)len_extra_val, len_extra_bits);
    }

    int dist_sym = cz_dist_to_sym(match_dist);
    int dist_extra_bits = cz_dist_extra[dist_sym];
    int dist_extra_val = match_dist - cz_dist_base[dist_sym];

    cz_get_fixed_dist_code(dist_sym, &code, &len);
    cz_put_bits(b, code, len);
    if (dist_extra_bits > 0) {
        cz_put_bits(b, (uint32_t)dist_extra_val, dist_extra_bits);
    }
}

/* --- Stored-Block Fallback Encoder --- */

static long long cz_deflate_stored(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, int format) {
    size_t out_pos = 0;
    if (format == 1) {
        if (out_pos + 2 > out_cap) return -1;
        out[out_pos++] = 0x78;
        out[out_pos++] = 0x01;
    } else if (format == 2) {
        if (out_pos + 10 > out_cap) return -1;
        out[out_pos++] = 0x1f; out[out_pos++] = 0x8b; out[out_pos++] = 0x08; out[out_pos++] = 0x00;
        out[out_pos++] = 0x00; out[out_pos++] = 0x00; out[out_pos++] = 0x00; out[out_pos++] = 0x00;
        out[out_pos++] = 0x00; out[out_pos++] = 0xff;
    }

    size_t cur = 0;
    do {
        size_t chunk = in_len - cur;
        if (chunk > 65535) chunk = 65535;
        int bfinal = (cur + chunk >= in_len) ? 1 : 0;
        if (out_pos + 5 + chunk > out_cap) return -1;

        out[out_pos++] = (uint8_t)bfinal;
        out[out_pos++] = (uint8_t)(chunk & 0xFF);
        out[out_pos++] = (uint8_t)((chunk >> 8) & 0xFF);
        uint16_t nlen = (uint16_t)(~((uint16_t)chunk));
        out[out_pos++] = (uint8_t)(nlen & 0xFF);
        out[out_pos++] = (uint8_t)((nlen >> 8) & 0xFF);
        if (chunk > 0) {
            memcpy(out + out_pos, in + cur, chunk);
            out_pos += chunk;
            cur += chunk;
        }
    } while (cur < in_len);

    if (format == 1) {
        if (out_pos + 4 > out_cap) return -1;
        uint32_t adler = cz_adler32(in, in_len);
        out[out_pos++] = (uint8_t)((adler >> 24) & 0xFF);
        out[out_pos++] = (uint8_t)((adler >> 16) & 0xFF);
        out[out_pos++] = (uint8_t)((adler >> 8) & 0xFF);
        out[out_pos++] = (uint8_t)(adler & 0xFF);
    } else if (format == 2) {
        if (out_pos + 8 > out_cap) return -1;
        uint32_t crc = cz_crc32(in, in_len);
        uint32_t isize = (uint32_t)(in_len & 0xFFFFFFFFU);
        out[out_pos++] = (uint8_t)(crc & 0xFF);
        out[out_pos++] = (uint8_t)((crc >> 8) & 0xFF);
        out[out_pos++] = (uint8_t)((crc >> 16) & 0xFF);
        out[out_pos++] = (uint8_t)((crc >> 24) & 0xFF);
        out[out_pos++] = (uint8_t)(isize & 0xFF);
        out[out_pos++] = (uint8_t)((isize >> 8) & 0xFF);
        out[out_pos++] = (uint8_t)((isize >> 16) & 0xFF);
        out[out_pos++] = (uint8_t)((isize >> 24) & 0xFF);
    }

    return (long long)out_pos;
}

/* --- Dynamic Huffman Types & Helpers --- */

typedef struct {
    uint16_t len;  /* 0..255 for literal, 3..258 for match length */
    uint16_t dist; /* 0 for literal, 1..32768 for match distance */
} cz_token_t;

typedef struct {
    uint32_t freq;
    int16_t left;
    int16_t right;
    int16_t parent;
} cz_huff_node_t;

static void cz_build_huffman_lengths(const uint32_t *freq_in, int num_symbols, uint8_t *lens, int max_bits) {
    uint32_t freq[288];
    memcpy(freq, freq_in, (size_t)num_symbols * sizeof(uint32_t));

    for (;;) {
        cz_huff_node_t nodes[600];
        memset(nodes, 0, sizeof(nodes));

        int heap[300];
        int heap_size = 0;

        for (int i = 0; i < num_symbols; i++) {
            if (freq[i] > 0) {
                nodes[i].freq = freq[i];
                nodes[i].left = -1;
                nodes[i].right = -1;
                nodes[i].parent = -1;

                int cur = heap_size++;
                heap[cur] = i;
                while (cur > 0) {
                    int p = (cur - 1) / 2;
                    if (nodes[heap[cur]].freq < nodes[heap[p]].freq) {
                        int tmp = heap[cur]; heap[cur] = heap[p]; heap[p] = tmp;
                        cur = p;
                    } else break;
                }
            }
        }

        if (heap_size <= 1) {
            memset(lens, 0, (size_t)num_symbols);
            if (heap_size == 1) lens[heap[0]] = 1;
            return;
        }

        int next_node = num_symbols;

        while (heap_size > 1) {
            int a = heap[0];
            heap[0] = heap[--heap_size];
            int cur = 0;
            while (2 * cur + 1 < heap_size) {
                int left = 2 * cur + 1;
                int right = left + 1;
                int smallest = (right < heap_size && nodes[heap[right]].freq < nodes[heap[left]].freq) ? right : left;
                if (nodes[heap[smallest]].freq < nodes[heap[cur]].freq) {
                    int tmp = heap[cur]; heap[cur] = heap[smallest]; heap[smallest] = tmp;
                    cur = smallest;
                } else break;
            }

            int b = heap[0];
            heap[0] = heap[--heap_size];
            cur = 0;
            while (2 * cur + 1 < heap_size) {
                int left = 2 * cur + 1;
                int right = left + 1;
                int smallest = (right < heap_size && nodes[heap[right]].freq < nodes[heap[left]].freq) ? right : left;
                if (nodes[heap[smallest]].freq < nodes[heap[cur]].freq) {
                    int tmp = heap[cur]; heap[cur] = heap[smallest]; heap[smallest] = tmp;
                    cur = smallest;
                } else break;
            }

            int p = next_node++;
            nodes[p].freq = nodes[a].freq + nodes[b].freq;
            nodes[p].left = (int16_t)a;
            nodes[p].right = (int16_t)b;
            nodes[p].parent = -1;
            nodes[a].parent = (int16_t)p;
            nodes[b].parent = (int16_t)p;

            cur = heap_size++;
            heap[cur] = p;
            while (cur > 0) {
                int parent = (cur - 1) / 2;
                if (nodes[heap[cur]].freq < nodes[heap[parent]].freq) {
                    int tmp = heap[cur]; heap[cur] = heap[parent]; heap[parent] = tmp;
                    cur = parent;
                } else break;
            }
        }

        int max_depth = 0;
        for (int i = 0; i < num_symbols; i++) {
            if (freq_in[i] == 0) {
                lens[i] = 0;
                continue;
            }
            int d = 0;
            int cur = i;
            while (nodes[cur].parent != -1) {
                d++;
                cur = nodes[cur].parent;
            }
            lens[i] = (uint8_t)d;
            if (d > max_depth) max_depth = d;
        }

        if (max_depth <= max_bits) {
            return;
        }

        for (int i = 0; i < num_symbols; i++) {
            if (freq[i] > 0) {
                freq[i] = (freq[i] >> 1) ? (freq[i] >> 1) : 1;
            }
        }
    }
}

static void cz_gen_canonical_codes(const uint8_t *lens, int num_symbols, uint16_t *codes) {
    int count[16] = {0};
    for (int i = 0; i < num_symbols; i++) count[lens[i]]++;
    count[0] = 0;

    int code = 0;
    int next_code[16] = {0};
    for (int bits = 1; bits <= 15; bits++) {
        code = (code + count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    for (int i = 0; i < num_symbols; i++) {
        int len = lens[i];
        if (len == 0) {
            codes[i] = 0;
            continue;
        }
        int c = next_code[len]++;
        uint16_t rev = 0;
        for (int b = 0; b < len; b++) {
            rev |= (((uint16_t)((c >> b) & 1)) << (len - 1 - b));
        }
        codes[i] = rev;
    }
}

typedef struct {
    uint8_t sym;
    uint8_t extra_bits;
    uint8_t extra_val;
} cz_rle_token_t;

static void cz_emit_block(cz_out_bits_t *b, const uint8_t *in, size_t raw_start, size_t raw_len,
                          const cz_token_t *tokens, int num_tokens,
                          uint32_t *lit_freq, uint32_t *dist_freq,
                          int bfinal, int level) {
    /* Ensure distance table has at least 1 symbol for compliance */
    int dist_count = 0;
    for (int i = 0; i < 32; i++) {
        if (dist_freq[i] > 0) dist_count++;
    }
    if (dist_count == 0) {
        dist_freq[0] = 1;
    }

    /* 1. Build Dynamic Trees */
    uint8_t lit_lens[288];
    cz_build_huffman_lengths(lit_freq, 288, lit_lens, 15);
    uint8_t dist_lens[32];
    cz_build_huffman_lengths(dist_freq, 32, dist_lens, 15);

    int hlit = 286;
    while (hlit > 257 && lit_lens[hlit - 1] == 0) hlit--;
    int hdist = 32;
    while (hdist > 1 && dist_lens[hdist - 1] == 0) hdist--;

    int total_codes = hlit + hdist;
    uint8_t combined_lens[320];
    memcpy(combined_lens, lit_lens, (size_t)hlit);
    memcpy(combined_lens + hlit, dist_lens, (size_t)hdist);

    cz_rle_token_t rle[350];
    int rle_count = 0;

    int idx = 0;
    while (idx < total_codes) {
        uint8_t val = combined_lens[idx];
        int run = 1;
        while (idx + run < total_codes && combined_lens[idx + run] == val) {
            run++;
        }

        if (val == 0) {
            while (run >= 11) {
                int chunk = run > 138 ? 138 : run;
                rle[rle_count++] = (cz_rle_token_t){18, 7, (uint8_t)(chunk - 11)};
                idx += chunk;
                run -= chunk;
            }
            if (run >= 3) {
                int chunk = run > 10 ? 10 : run;
                rle[rle_count++] = (cz_rle_token_t){17, 3, (uint8_t)(chunk - 3)};
                idx += chunk;
                run -= chunk;
            }
            while (run > 0) {
                rle[rle_count++] = (cz_rle_token_t){0, 0, 0};
                idx++;
                run--;
            }
        } else {
            rle[rle_count++] = (cz_rle_token_t){val, 0, 0};
            idx++;
            run--;
            while (run >= 3) {
                int chunk = run > 6 ? 6 : run;
                rle[rle_count++] = (cz_rle_token_t){16, 2, (uint8_t)(chunk - 3)};
                idx += chunk;
                run -= chunk;
            }
            while (run > 0) {
                rle[rle_count++] = (cz_rle_token_t){val, 0, 0};
                idx++;
                run--;
            }
        }
    }

    uint32_t cl_freq[19] = {0};
    for (int k = 0; k < rle_count; k++) {
        cl_freq[rle[k].sym]++;
    }
    uint8_t cl_lens[19];
    cz_build_huffman_lengths(cl_freq, 19, cl_lens, 7);
    uint16_t cl_codes[19];
    cz_gen_canonical_codes(cl_lens, 19, cl_codes);

    int hclen = 19;
    while (hclen > 4 && cl_lens[cz_cl_order[hclen - 1]] == 0) {
        hclen--;
    }

    /* 2. Calculate bit cost of Dynamic block */
    uint32_t dynamic_bits = 3 + 5 + 5 + 4 + (uint32_t)hclen * 3;
    for (int k = 0; k < rle_count; k++) {
        dynamic_bits += (uint32_t)cl_lens[rle[k].sym] + (uint32_t)rle[k].extra_bits;
    }
    for (int s = 0; s < 288; s++) {
        if (lit_freq[s] > 0) dynamic_bits += lit_freq[s] * (uint32_t)lit_lens[s];
    }
    for (int s = 0; s < 32; s++) {
        if (dist_freq[s] > 0) dynamic_bits += dist_freq[s] * ((uint32_t)dist_lens[s] + (uint32_t)cz_dist_extra[s]);
    }
    for (int t = 0; t < num_tokens; t++) {
        if (tokens[t].dist > 0) {
            int lsym = cz_len_to_sym(tokens[t].len);
            dynamic_bits += (uint32_t)cz_len_extra[lsym - 257];
        }
    }

    /* 3. Calculate bit cost of Fixed block */
    uint32_t fixed_bits = 3;
    for (int s = 0; s < 288; s++) {
        if (lit_freq[s] > 0) {
            int l = (s <= 143) ? 8 : (s <= 255) ? 9 : (s <= 279) ? 7 : 8;
            fixed_bits += lit_freq[s] * (uint32_t)l;
        }
    }
    for (int s = 0; s < 32; s++) {
        if (dist_freq[s] > 0) {
            fixed_bits += dist_freq[s] * (5 + (uint32_t)cz_dist_extra[s]);
        }
    }
    for (int t = 0; t < num_tokens; t++) {
        if (tokens[t].dist > 0) {
            int lsym = cz_len_to_sym(tokens[t].len);
            fixed_bits += (uint32_t)cz_len_extra[lsym - 257];
        }
    }

    /* 4. Calculate bit cost of Stored block */
    uint32_t stored_bits = (raw_len <= 65535) ? (40 + (uint32_t)raw_len * 8) : (uint32_t)(40 * ((raw_len + 65534) / 65535) + raw_len * 8);

    /* Choose encoding */
    int use_dynamic = (level >= 4 && dynamic_bits <= fixed_bits && dynamic_bits <= stored_bits);
    int use_fixed = (!use_dynamic && fixed_bits <= stored_bits);

    if (use_dynamic) {
        uint16_t lit_codes[288];
        cz_gen_canonical_codes(lit_lens, 288, lit_codes);
        uint16_t dist_codes[32];
        cz_gen_canonical_codes(dist_lens, 32, dist_codes);

        /* Block Header: BFINAL, BTYPE=10 */
        cz_put_bits(b, (uint32_t)((bfinal ? 1 : 0) | (2 << 1)), 3);
        cz_put_bits(b, (uint32_t)(hlit - 257), 5);
        cz_put_bits(b, (uint32_t)(hdist - 1), 5);
        cz_put_bits(b, (uint32_t)(hclen - 4), 4);

        for (int j = 0; j < hclen; j++) {
            cz_put_bits(b, cl_lens[cz_cl_order[j]], 3);
        }
        for (int k = 0; k < rle_count; k++) {
            cz_put_bits(b, cl_codes[rle[k].sym], cl_lens[rle[k].sym]);
            if (rle[k].extra_bits > 0) {
                cz_put_bits(b, rle[k].extra_val, rle[k].extra_bits);
            }
        }

        /* Emit Tokens */
        for (int t = 0; t < num_tokens; t++) {
            if (tokens[t].dist == 0) {
                int c = (int)tokens[t].len;
                cz_put_bits(b, lit_codes[c], lit_lens[c]);
            } else {
                int lsym = cz_len_to_sym(tokens[t].len);
                int lextra = cz_len_extra[lsym - 257];
                int lval = (int)tokens[t].len - cz_len_base[lsym - 257];

                cz_put_bits(b, lit_codes[lsym], lit_lens[lsym]);
                if (lextra > 0) cz_put_bits(b, (uint32_t)lval, lextra);

                int dsym = cz_dist_to_sym(tokens[t].dist);
                int dextra = cz_dist_extra[dsym];
                int dval = (int)tokens[t].dist - cz_dist_base[dsym];

                cz_put_bits(b, dist_codes[dsym], dist_lens[dsym]);
                if (dextra > 0) cz_put_bits(b, (uint32_t)dval, dextra);
            }
        }
        /* EOB */
        cz_put_bits(b, lit_codes[256], lit_lens[256]);
    } else if (use_fixed) {
        /* Block Header: BFINAL, BTYPE=01 */
        cz_put_bits(b, (uint32_t)((bfinal ? 1 : 0) | (1 << 1)), 3);

        for (int t = 0; t < num_tokens; t++) {
            if (tokens[t].dist == 0) {
                cz_emit_fixed_lit(b, (int)tokens[t].len);
            } else {
                cz_emit_fixed_match(b, (int)tokens[t].len, (int)tokens[t].dist);
            }
        }
        cz_emit_fixed_lit(b, 256);
    } else {
        /* Stored Block */
        size_t scur = 0;
        do {
            size_t schunk = raw_len - scur;
            if (schunk > 65535) schunk = 65535;
            int sfinal = (bfinal && (scur + schunk >= raw_len)) ? 1 : 0;

            cz_put_bits(b, (uint32_t)(sfinal ? 1 : 0), 3); /* BFINAL + BTYPE=00 */
            cz_put_flush(b);

            if (b->out_pos + 4 + schunk > b->out_cap) {
                b->overflow = 1;
                return;
            }
            b->out[b->out_pos++] = (uint8_t)(schunk & 0xFF);
            b->out[b->out_pos++] = (uint8_t)((schunk >> 8) & 0xFF);
            uint16_t snlen = (uint16_t)(~((uint16_t)schunk));
            b->out[b->out_pos++] = (uint8_t)(snlen & 0xFF);
            b->out[b->out_pos++] = (uint8_t)((snlen >> 8) & 0xFF);
            if (schunk > 0) {
                memcpy(b->out + b->out_pos, in + raw_start + scur, schunk);
                b->out_pos += schunk;
                scur += schunk;
            }
        } while (scur < raw_len);
    }
}

/* --- DEFLATE --- */

#define CZ_HASH_BITS 15
#define CZ_HASH_SIZE (1 << CZ_HASH_BITS)
#define CZ_WINDOW_SIZE 32768
#define CZ_WINDOW_MASK (CZ_WINDOW_SIZE - 1)
#define CZ_BLOCK_TOKENS 16384

static inline uint32_t cz_hash3(const uint8_t *p) {
    return ((((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ (uint32_t)p[2]) * 2654435761U) >> (32 - CZ_HASH_BITS);
}

long long salivo_deflate(long long in_addr, long long in_len, long long out_addr, long long out_cap, long long format, long long level) {
    if (in_len < 0 || out_cap < 0) return -1;
    if (in_addr == 0 && in_len > 0) return -1;
    if (out_addr == 0 && out_cap > 0) return -1;
    if (format < 0 || format > 2) return -1;
    if (level < 0) return -1;

    const uint8_t *in = (const uint8_t *)(uintptr_t)in_addr;
    uint8_t *out = (uint8_t *)(uintptr_t)out_addr;

    /* Stored block max size calculation */
    size_t stored_overhead = (size_t)in_len + 5 * (((size_t)in_len + 65534) / 65535);
    if (format == 1) stored_overhead += 6;
    else if (format == 2) stored_overhead += 18;

    if (level == 0) {
        return cz_deflate_stored(in, (size_t)in_len, out, (size_t)out_cap, (int)format);
    }

    cz_out_bits_t b;
    b.out = out;
    b.out_cap = (size_t)out_cap;
    b.out_pos = 0;
    b.bit_buf = 0;
    b.bit_cnt = 0;
    b.overflow = 0;

    /* Format Header */
    if (format == 1) {
        if (b.out_pos + 2 > b.out_cap) return -1;
        b.out[b.out_pos++] = 0x78;
        b.out[b.out_pos++] = (level < 6) ? 0x5E : 0x9C;
    } else if (format == 2) {
        if (b.out_pos + 10 > b.out_cap) return -1;
        b.out[b.out_pos++] = 0x1f; b.out[b.out_pos++] = 0x8b; b.out[b.out_pos++] = 0x08; b.out[b.out_pos++] = 0x00;
        b.out[b.out_pos++] = 0x00; b.out[b.out_pos++] = 0x00; b.out[b.out_pos++] = 0x00; b.out[b.out_pos++] = 0x00;
        b.out[b.out_pos++] = (level == 9) ? 0x02 : (level == 1) ? 0x04 : 0x00;
        b.out[b.out_pos++] = 0xff;
    }

    /* Allocate hash chains and token buffer */
    int32_t *head = (int32_t *)malloc(CZ_HASH_SIZE * sizeof(int32_t));
    int32_t *prev = (int32_t *)malloc(CZ_WINDOW_SIZE * sizeof(int32_t));
    cz_token_t *tokens = (cz_token_t *)malloc(CZ_BLOCK_TOKENS * sizeof(cz_token_t));
    if (!head || !prev || !tokens) {
        if (head) free(head);
        if (prev) free(prev);
        if (tokens) free(tokens);
        return -1;
    }
    memset(head, 0xFF, CZ_HASH_SIZE * sizeof(int32_t));
    memset(prev, 0xFF, CZ_WINDOW_SIZE * sizeof(int32_t));

    int max_chain = (level >= 9) ? 256 : (level >= 6) ? 32 : (level >= 4) ? 16 : 4;
    int nice_len = (level >= 6) ? 128 : 32;

    int num_tokens = 0;
    size_t raw_block_start = 0;
    uint32_t lit_freq[288] = {0};
    uint32_t dist_freq[32] = {0};

    size_t cur = 0;
    while (cur < (size_t)in_len) {
        if (b.overflow || b.out_pos > stored_overhead) {
            break;
        }

        /* Check if current block buffer is full */
        if (num_tokens >= CZ_BLOCK_TOKENS) {
            lit_freq[256]++;
            cz_emit_block(&b, in, raw_block_start, cur - raw_block_start,
                          tokens, num_tokens, lit_freq, dist_freq, 0, (int)level);
            num_tokens = 0;
            raw_block_start = cur;
            memset(lit_freq, 0, sizeof(lit_freq));
            memset(dist_freq, 0, sizeof(dist_freq));
        }

        if (cur + 2 >= (size_t)in_len) {
            tokens[num_tokens].len = in[cur];
            tokens[num_tokens].dist = 0;
            lit_freq[in[cur]]++;
            num_tokens++;
            cur++;
            continue;
        }

        uint32_t h = cz_hash3(in + cur);
        int match_pos = head[h];
        prev[cur & CZ_WINDOW_MASK] = match_pos;
        head[h] = (int)cur;

        int best_len = 2;
        int best_dist = 0;
        int chain_len = 0;
        int max_possible = (int)(in_len - cur);
        if (max_possible > 258) max_possible = 258;

        while (match_pos >= 0 && (int)cur - match_pos <= CZ_WINDOW_SIZE && chain_len < max_chain &&
               best_len < max_possible) {
            chain_len++;
            if (in[match_pos + best_len] == in[cur + best_len] && in[match_pos] == in[cur]) {
                int l = 0;
                while (l < max_possible && in[match_pos + l] == in[cur + l]) {
                    l++;
                }
                if (l > best_len) {
                    best_len = l;
                    best_dist = (int)cur - match_pos;
                    if (best_len >= nice_len) break;
                }
            }
            match_pos = prev[match_pos & CZ_WINDOW_MASK];
        }

        /* Lazy matching for level >= 6 */
        if (best_len >= 3 && level >= 6 && cur + 1 + 2 < (size_t)in_len && best_len < (int)(in_len - (cur + 1))) {
            uint32_t h_next = cz_hash3(in + cur + 1);
            int m_next = head[h_next];
            int best_next_len = best_len;
            int best_next_dist = 0;
            int chain_next = 0;
            int max_p_next = (int)(in_len - (cur + 1));
            if (max_p_next > 258) max_p_next = 258;

            while (m_next >= 0 && (int)(cur + 1) - m_next <= CZ_WINDOW_SIZE && chain_next < max_chain &&
                   best_next_len < max_p_next) {
                chain_next++;
                if (in[m_next + best_next_len] == in[cur + 1 + best_next_len] && in[m_next] == in[cur + 1]) {
                    int l = 0;
                    while (l < max_p_next && in[m_next + l] == in[cur + 1 + l]) {
                        l++;
                    }
                    if (l > best_next_len) {
                        best_next_len = l;
                        best_next_dist = (int)(cur + 1) - m_next;
                        if (best_next_len >= nice_len) break;
                    }
                }
                m_next = prev[m_next & CZ_WINDOW_MASK];
            }

            if (best_next_len > best_len) {
                tokens[num_tokens].len = in[cur];
                tokens[num_tokens].dist = 0;
                lit_freq[in[cur]]++;
                num_tokens++;
                cur++;

                prev[cur & CZ_WINDOW_MASK] = head[h_next];
                head[h_next] = (int)cur;

                tokens[num_tokens].len = (uint16_t)best_next_len;
                tokens[num_tokens].dist = (uint16_t)best_next_dist;
                int lsym = cz_len_to_sym(best_next_len);
                int dsym = cz_dist_to_sym(best_next_dist);
                lit_freq[lsym]++;
                dist_freq[dsym]++;
                num_tokens++;

                for (int k = 1; k < best_next_len; k++) {
                    if (cur + k + 2 < (size_t)in_len) {
                        uint32_t hk = cz_hash3(in + cur + k);
                        prev[(cur + k) & CZ_WINDOW_MASK] = head[hk];
                        head[hk] = (int)(cur + k);
                    }
                }
                cur += (size_t)best_next_len;
                continue;
            }
        }

        if (best_len >= 3) {
            tokens[num_tokens].len = (uint16_t)best_len;
            tokens[num_tokens].dist = (uint16_t)best_dist;
            int lsym = cz_len_to_sym(best_len);
            int dsym = cz_dist_to_sym(best_dist);
            lit_freq[lsym]++;
            dist_freq[dsym]++;
            num_tokens++;

            for (int k = 1; k < best_len; k++) {
                if (cur + k + 2 < (size_t)in_len) {
                    uint32_t hk = cz_hash3(in + cur + k);
                    prev[(cur + k) & CZ_WINDOW_MASK] = head[hk];
                    head[hk] = (int)(cur + k);
                }
            }
            cur += (size_t)best_len;
        } else {
            tokens[num_tokens].len = in[cur];
            tokens[num_tokens].dist = 0;
            lit_freq[in[cur]]++;
            num_tokens++;
            cur++;
        }
    }

    free(head);
    free(prev);

    /* Final Block */
    lit_freq[256]++;
    cz_emit_block(&b, in, raw_block_start, (size_t)in_len - raw_block_start,
                  tokens, num_tokens, lit_freq, dist_freq, 1, (int)level);
    free(tokens);

    cz_put_flush(&b);

    /* Trailer */
    if (format == 1) {
        if (b.out_pos + 4 > b.out_cap) b.overflow = 1;
        else {
            uint32_t adler = cz_adler32(in, in_len);
            b.out[b.out_pos++] = (uint8_t)((adler >> 24) & 0xFF);
            b.out[b.out_pos++] = (uint8_t)((adler >> 16) & 0xFF);
            b.out[b.out_pos++] = (uint8_t)((adler >> 8) & 0xFF);
            b.out[b.out_pos++] = (uint8_t)(adler & 0xFF);
        }
    } else if (format == 2) {
        if (b.out_pos + 8 > b.out_cap) b.overflow = 1;
        else {
            uint32_t crc = cz_crc32(in, in_len);
            uint32_t isize = (uint32_t)(in_len & 0xFFFFFFFFU);
            b.out[b.out_pos++] = (uint8_t)(crc & 0xFF);
            b.out[b.out_pos++] = (uint8_t)((crc >> 8) & 0xFF);
            b.out[b.out_pos++] = (uint8_t)((crc >> 16) & 0xFF);
            b.out[b.out_pos++] = (uint8_t)((crc >> 24) & 0xFF);
            b.out[b.out_pos++] = (uint8_t)(isize & 0xFF);
            b.out[b.out_pos++] = (uint8_t)((isize >> 8) & 0xFF);
            b.out[b.out_pos++] = (uint8_t)((isize >> 16) & 0xFF);
            b.out[b.out_pos++] = (uint8_t)((isize >> 24) & 0xFF);
        }
    }

    /* Never produce output larger than stored block would */
    if (b.overflow || b.out_pos > stored_overhead) {
        return cz_deflate_stored(in, (size_t)in_len, out, (size_t)out_cap, (int)format);
    }

    return (long long)b.out_pos;
}

#endif /* SALIVO_COMPRESS_INC */
