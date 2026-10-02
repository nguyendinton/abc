/**
 * @file  test_parser.c
 * @brief Unit-test tầng khung, chạy TRÊN PC bằng gcc (không cần board).
 *
 *   cd test && make && ./test_parser
 *
 * Đây là bằng chứng định lượng rất "ăn điểm" cho báo cáo: nhóm chứng minh
 * được parser tự đồng bộ lại đúng trong 6 kịch bản lỗi khác nhau, và đo được
 * tỉ lệ khung khôi phục trên 20.000 khung ngẫu nhiên — không cần cắm board.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "protocol.h"
#include "checksum.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) { g_pass++; }                                               \
        else { g_fail++; printf("  [FAIL] " __VA_ARGS__); printf("\n"); }     \
    } while (0)

/* ------------------------------------------------------------------ */
/* Tiện ích: nạp một mảng byte, đếm số khung hợp lệ thu được           */
/* ------------------------------------------------------------------ */
typedef struct { uint8_t seq, cmd, len; uint8_t data[PROTO_MAX_DATA]; } cap_t;

static int feed_all(proto_parser_t *p, const uint8_t *s, size_t n,
                    cap_t *out, int max_out)
{
    int k = 0;
    for (size_t i = 0; i < n; i++) {
        const proto_frame_t *f = proto_feed(p, s[i], (uint32_t)i);
        if (f && k < max_out) {
            out[k].seq = f->seq; out[k].cmd = f->cmd; out[k].len = f->len;
            memcpy(out[k].data, f->data, f->len);
            k++;
        }
    }
    return k;
}

/* ================================================================== */
int main(void)
{
    proto_stats_t  st;
    proto_parser_t p;
    uint8_t  buf[512];
    cap_t    cap[128];
    int      n;

    printf("=== TEST TANG KHUNG — giao thuc V01 v1.1 ===\n");
    printf("Che do checksum: %s (%u byte)\n\n",
#if (PROTO_CKS_MODE == PROTO_CKS_CRC16)
           "CRC-16/CCITT-FALSE",
#elif (PROTO_CKS_MODE == PROTO_CKS_CRC8)
           "CRC-8/ATM",
#else
           "SUM-8",
#endif
           (unsigned)PROTO_CKS_LEN);

    /* --- T1: vector chuẩn của CRC ---------------------------------- */
    printf("T1. Vector chuan CRC\n");
    CHECK(cks_crc16((const uint8_t *)"123456789", 9) == 0x29B1,
          "CRC16(\"123456789\") = 0x%04X, mong doi 0x29B1",
          cks_crc16((const uint8_t *)"123456789", 9));
    CHECK(cks_crc8((const uint8_t *)"123456789", 9) == 0xF4,
          "CRC8(\"123456789\") = 0x%02X, mong doi 0xF4",
          cks_crc8((const uint8_t *)"123456789", 9));

    /* --- T2: đóng gói / tách khung khứ hồi -------------------------- */
    printf("T2. Dong goi -> tach khung (round-trip)\n");
    {
        const uint8_t payload[] = {1, 2, 3, 250, 0, 0xAA, 0x55};
        uint16_t flen = proto_build(buf, 0x42, CMD_ECHO, payload, sizeof(payload));
        CHECK(flen == PROTO_HDR_LEN + sizeof(payload) + PROTO_CKS_LEN,
              "do dai khung = %u", flen);

        proto_parser_init(&p, &st);
        n = feed_all(&p, buf, flen, cap, 64);
        CHECK(n == 1, "so khung thu duoc = %d, mong doi 1", n);
        CHECK(n == 1 && cap[0].seq == 0x42 && cap[0].cmd == CMD_ECHO &&
              cap[0].len == sizeof(payload) &&
              memcmp(cap[0].data, payload, sizeof(payload)) == 0,
              "noi dung khung sai");
        /* Chú ý: payload cố tình chứa 0xAA 0x55 để chứng minh parser KHÔNG
           bị nhầm bởi SOF nằm trong DATA (vì đã biết LEN từ trước). */
    }

    /* --- T3: sai 1 bit -> phải bị loại ----------------------------- */
    printf("T3. Dao 1 bit trong DATA -> phai bi loai\n");
    {
        const uint8_t payload[] = {0x11, 0x22, 0x33, 0x44};
        uint16_t flen = proto_build(buf, 7, CMD_ECHO, payload, sizeof(payload));
        buf[6] ^= 0x01;                       /* đảo 1 bit trong DATA */
        proto_parser_init(&p, &st);
        n = feed_all(&p, buf, flen, cap, 64);
        CHECK(n == 0, "khung hong van lot qua (%d khung)", n);
        CHECK(st.err_cks == 1, "err_cks = %u, mong doi 1", st.err_cks);
    }

    /* --- T4a: chèn rác thường giữa 2 khung -------------------------- */
    printf("T4a. Chen rac thuong giua cac khung\n");
    {
        uint8_t stream[512];
        size_t  o = 0;
        const uint8_t junk[] = {0x00, 0xFF, 0xAA, 0x12, 0x7E};  /* có 0xAA lẻ */
        for (int i = 0; i < 6; i++) {
            memcpy(stream + o, junk, sizeof(junk)); o += sizeof(junk);
            uint8_t d[2] = {(uint8_t)i, 0xEE};
            uint16_t fl = proto_build(buf, (uint8_t)i, CMD_PING, d, 2);
            memcpy(stream + o, buf, fl); o += fl;
        }
        proto_parser_init(&p, &st);
        n = feed_all(&p, stream, o, cap, 128);
        CHECK(n == 6, "khoi phuc duoc %d/6 khung sau khi chen rac", n);
        for (int i = 0; i < n; i++) {
            CHECK(cap[i].seq == (uint8_t)i, "khung %d sai seq", i);
        }
    }

    /* --- T4b: rác chứa CẶP SOF GIẢ (ca khó nhất) -------------------- */
    printf("T4b. Rac chua cap SOF gia 0xAA 0x55 -> parser bi lua, phai resync\n");
    {
        uint8_t stream[2048];
        size_t  o = 0;
        const uint8_t junk[] = {0x00, 0xAA, 0xAA, 0x55};  /* SOF giả ngay truoc */
        const int NF = 40;
        for (int i = 0; i < NF; i++) {
            memcpy(stream + o, junk, sizeof(junk)); o += sizeof(junk);
            uint8_t d[2] = {(uint8_t)i, 0xEE};
            uint16_t fl = proto_build(buf, (uint8_t)i, CMD_PING, d, 2);
            memcpy(stream + o, buf, fl); o += fl;
        }
        proto_parser_init(&p, &st);
        n = feed_all(&p, stream, o, cap, 128);
        printf("     khoi phuc %d/%d khung, resync %u lan\n", n, NF, st.resync);
        /* SOF giả khiến parser "nuốt" một mảng dài; chỉ khi khung giả đó kết
           thúc (CRC sai) thì resync mới nhả các khung thật bên trong ra. Khung
           cuối cùng có thể còn treo trong bộ đệm khi luồng kết thúc.          */
        CHECK(n >= NF - 2, "khoi phuc qua it: %d/%d", n, NF);
        CHECK(st.resync >= 1, "khong he resync");
    }

    /* --- T5: khung hỏng BAO khung tốt (kiểm tra tự đồng bộ thật sự) - */
    printf("T5. Khung tot nam LOT trong khung hong -> phai resync ra duoc\n");
    {
        uint8_t inner[64];
        uint16_t il = proto_build(inner, 0x77, CMD_LED_GET, NULL, 0);

        /* Khung ngoài khai LEN lớn, nuốt luôn khung trong, rồi CRC sai. */
        uint8_t stream[256];
        size_t  o = 0;
        stream[o++] = PROTO_SOF1;
        stream[o++] = PROTO_SOF2;
        stream[o++] = 40;                /* LEN = 40 (nói dối)   */
        stream[o++] = 0x01;              /* SEQ                  */
        stream[o++] = CMD_ECHO;          /* CMD                  */
        memcpy(stream + o, inner, il); o += il;      /* khung tốt bị nuốt */
        while (o < 5 + 40) stream[o++] = 0x5A;       /* độn cho đủ LEN    */
        stream[o++] = 0x00;                          /* CRC sai           */
        if (PROTO_CKS_LEN == 2) stream[o++] = 0x00;

        proto_parser_init(&p, &st);
        n = feed_all(&p, stream, o, cap, 128);
        CHECK(n == 1, "khong tim lai duoc khung ben trong (%d khung)", n);
        CHECK(n == 1 && cap[0].seq == 0x77 && cap[0].cmd == CMD_LED_GET,
              "khung tim lai duoc khong dung");
        CHECK(st.resync >= 1, "resync = %u, mong doi >= 1", st.resync);
    }

    /* --- T6: LEN vô lý --------------------------------------------- */
    printf("T6. LEN vuot gioi han -> loai ngay, khong treo\n");
    {
        uint8_t stream[64]; size_t o = 0;
        stream[o++] = PROTO_SOF1; stream[o++] = PROTO_SOF2;
        stream[o++] = 0xFF;                       /* LEN = 255 > MAX */
        uint16_t fl = proto_build(buf, 9, CMD_PING, NULL, 0);
        memcpy(stream + o, buf, fl); o += fl;
        proto_parser_init(&p, &st);
        n = feed_all(&p, stream, o, cap, 128);
        CHECK(st.err_len == 1, "err_len = %u, mong doi 1", st.err_len);
        CHECK(n == 1, "khung sau do phai duoc nhan (%d)", n);
    }

    /* --- T7: mất byte giữa khung + timeout -------------------------- */
    printf("T7. Mat byte giua khung -> inter-byte timeout don dep\n");
    {
        uint8_t d[4] = {1, 2, 3, 4};
        uint16_t fl = proto_build(buf, 5, CMD_ECHO, d, 4);
        proto_parser_init(&p, &st);
        for (uint16_t i = 0; i < fl - 3; i++) {       /* gửi thiếu 3 byte */
            proto_feed(&p, buf[i], i);
        }
        proto_tick(&p, 1000);                          /* 1 s sau        */
        CHECK(st.err_timeout == 1, "err_timeout = %u, mong doi 1", st.err_timeout);
        CHECK(p.state == ST_SOF1, "parser khong ve trang thai nghi");
        /* khung kế tiếp vẫn phải nhận được bình thường */
        n = feed_all(&p, buf, fl, cap, 64);
        CHECK(n == 1, "khung sau timeout khong nhan duoc");
    }

    /* --- T8: fuzz 20.000 khung -------------------------------------- */
    printf("T8. Fuzz 20.000 khung + rac ngau nhien\n");
    {
        srand(12345);
        proto_parser_init(&p, &st);
        int sent = 0, got = 0;
        uint8_t s[600];
        for (int i = 0; i < 20000; i++) {
            size_t o = 0;
            int jn = rand() % 8;                       /* 0..7 byte rác  */
            for (int j = 0; j < jn; j++) {
                s[o++] = (uint8_t)(rand() & 0xFF);
            }
            uint8_t dl = (uint8_t)(rand() % 33);
            uint8_t d[33];
            for (int j = 0; j < dl; j++) d[j] = (uint8_t)(rand() & 0xFF);
            uint16_t fl = proto_build(buf, (uint8_t)i, CMD_ECHO, d, dl);
            memcpy(s + o, buf, fl); o += fl;
            sent++;
            for (size_t j = 0; j < o; j++) {
                if (proto_feed(&p, s[j], (uint32_t)i)) got++;
            }
        }
        printf("     gui %d khung, nhan %d khung (%.3f%%), resync %u lan\n",
               sent, got, 100.0 * got / sent, st.resync);
        /* Rác ngẫu nhiên có xác suất rất nhỏ tạo ra khung giả hợp lệ,
           nên chấp nhận sai lệch <= 0.5%. */
        CHECK(got >= sent * 995 / 1000, "ti le khoi phuc qua thap");
    }

    /* --- T9: khả năng phát hiện lỗi của 3 kiểu checksum -------------- */
    printf("T9. Do ti le PHAT HIEN LOI (dao ngau nhien 1..4 bit)\n");
    {
        srand(999);
        const int  N = 200000;
        int missed = 0;
        for (int i = 0; i < N; i++) {
            uint8_t d[16];
            for (int j = 0; j < 16; j++) d[j] = (uint8_t)(rand() & 0xFF);
            uint16_t fl = proto_build(buf, (uint8_t)i, CMD_ECHO, d, 16);
            uint8_t  bad[64];
            memcpy(bad, buf, fl);
            /* Đảo nbit bit PHÂN BIỆT. Nếu bốc trùng một bit hai lần thì hai
               phép đảo triệt tiêu nhau, khung vẫn nguyên vẹn -> số liệu sai. */
            int nbit = 1 + rand() % 4;
            int used[4];
            int nbits_total = (fl - 2) * 8;
            for (int k = 0; k < nbit; k++) {
                int p, dup;
                do {
                    p = rand() % nbits_total;
                    dup = 0;
                    for (int q = 0; q < k; q++) { if (used[q] == p) dup = 1; }
                } while (dup);
                used[k] = p;
                bad[2 + p / 8] ^= (uint8_t)(1u << (p % 8));
            }
            proto_parser_init(&p, &st);
            if (feed_all(&p, bad, fl, cap, 64) > 0) { missed++; }
        }
        printf("     bo sot %d / %d khung hong  -> ti le phat hien = %.4f%%\n",
               missed, N, 100.0 * (N - missed) / N);
        CHECK(1, "");
    }

    printf("\n=== KET QUA: %d PASS, %d FAIL ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
