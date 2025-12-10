 #include <stdio.h>
 #include <stdlib.h>
 #include <math.h>
 #include <string.h>
 #include <stdint.h>
 
 #define ABORT(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } while(0)
 #define ASSERT(TEST, ...) do { if (!(TEST)) ABORT(__VA_ARGS__); } while(0)
 
 #define P_SHIFT    12
 #define MAX_TOT    200000  
 #define INC_MAIN   90    
 #define INC_SIDE   20    
 
 #define STEP1 6
 #define STEP2 4
 #define STEP3 3
 #define STEP4 2
 #define STEP5 2
 #define STEP6 1
 #define STEP7 1
 #define STEP8 1
 
 typedef struct {
     uint32_t channels;
     uint32_t samplerate;
     uint32_t samples;
 } samples_t;
 
 #define WAV_CHUNK_ID(S) \
     (((uint32_t)(S[3])) << 24 | ((uint32_t)(S[2])) << 16 | \
      ((uint32_t)(S[1])) <<  8 | ((uint32_t)(S[0])))
 
 uint32_t fread_u32_le(FILE *fh) {
     uint8_t buf[4];
     if(fread(buf, 4, 1, fh) != 1) return 0;
     return (buf[3]<<24) | (buf[2]<<16) | (buf[1]<<8) | buf[0];
 }
 
 unsigned short fread_u16_le(FILE *fh) {
     uint8_t buf[2];
     if(fread(buf, 2, 1, fh) != 1) return 0;
     return (buf[1]<<8) | buf[0];
 }
 
 void fwrite_u32_le(uint32_t v, FILE *fh) {
     uint8_t buf[4] = {v, v>>8, v>>16, v>>24};
     fwrite(buf, 4, 1, fh);
 }
 
 short *wav_read(const char *path, samples_t *desc) {
     FILE *fh = fopen(path, "rb");
     ASSERT(fh, "Can't open %s", path);
     uint32_t chunk_type = fread_u32_le(fh);
     ASSERT(chunk_type == WAV_CHUNK_ID("RIFF"), "Not a RIFF container");
     fread_u32_le(fh); 
     uint32_t wavid = fread_u32_le(fh);
     ASSERT(wavid == WAV_CHUNK_ID("WAVE"), "No WAVE id");
     uint32_t data_sz = 0;
     while (1) {
         chunk_type = fread_u32_le(fh);
         uint32_t chunk_sz = fread_u32_le(fh);
         if (feof(fh)) break;
         if (chunk_type == WAV_CHUNK_ID("fmt ")) {
             fread_u16_le(fh);
             desc->channels = fread_u16_le(fh);
             desc->samplerate = fread_u32_le(fh);
             fread_u32_le(fh); fread_u16_le(fh); fread_u16_le(fh); 
             if (chunk_sz > 16) fseek(fh, chunk_sz - 16, SEEK_CUR);
         } else if (chunk_type == WAV_CHUNK_ID("data")) {
             data_sz = chunk_sz;
             break;
         } else {
             fseek(fh, chunk_sz, SEEK_CUR);
         }
     }
     ASSERT(data_sz > 0, "No data found");
     desc->samples = data_sz / (desc->channels * 2);
     short *data = malloc(data_sz);
     fread(data, 1, data_sz, fh);
     fclose(fh);
     return data;
 }
 
 typedef struct {
     uint8_t *buf;
     uint32_t buf_size;
     uint32_t buf_cap;
     uint64_t low;
     uint32_t range;
     uint8_t cache;
     uint32_t cache_size;
 } RangeEnc;
 
 void re_init(RangeEnc *rc) {
     rc->buf_cap = 1024 * 256;
     rc->buf = malloc(rc->buf_cap);
     rc->buf_size = 0;
     rc->low = 0;
     rc->range = 0xFFFFFFFF;
     rc->cache = 0;
     rc->cache_size = 0;
 }
 
 void re_out_byte(RangeEnc *rc, uint8_t b) {
     if (rc->buf_size >= rc->buf_cap) {
         rc->buf_cap *= 2;
         rc->buf = realloc(rc->buf, rc->buf_cap);
     }
     rc->buf[rc->buf_size++] = b;
 }
 
 void re_normalize(RangeEnc *rc) {
     while (rc->range < (1u << 24)) {
         if ((rc->low >> 24) != 0xFF) {
             re_out_byte(rc, rc->cache + (rc->low >> 32));
             while (rc->cache_size > 0) {
                 re_out_byte(rc, 0xFF + (rc->low >> 32));
                 rc->cache_size--;
             }
             rc->cache = (rc->low >> 24) & 0xFF;
         } else {
             rc->cache_size++;
         }
         rc->low = (rc->low & 0xFFFFFF) << 8;
         rc->range <<= 8;
     }
 }
 
 void re_encode(RangeEnc *rc, uint32_t cum, uint32_t freq, uint32_t tot) {
     uint32_t r = rc->range / tot;
     rc->low += (uint64_t)cum * r;
     rc->range = freq * r;
     re_normalize(rc);
 }
 
 void re_flush(RangeEnc *rc) {
     for (int i = 0; i < 5; i++) {
         if ((rc->low >> 24) != 0xFF) {
             re_out_byte(rc, rc->cache + (rc->low >> 32));
             while (rc->cache_size > 0) {
                 re_out_byte(rc, 0xFF + (rc->low >> 32));
                 rc->cache_size--;
             }
             rc->cache = (rc->low >> 24) & 0xFF;
         } else {
             rc->cache_size++;
         }
         rc->low = (rc->low & 0xFFFFFF) << 8;
     }
     re_out_byte(rc, rc->cache);
     while (rc->cache_size > 0) {
         re_out_byte(rc, 0xFF);
         rc->cache_size--;
     }
 }
 
 #define MAX_VAL 256
 #define ESCAPE_SYM 255
 #define CTX_COUNT 4
 
 typedef struct {
     uint32_t f[CTX_COUNT][MAX_VAL];
     uint32_t t[CTX_COUNT];
 } Model;
 
 int to_sym(int v) {
     if (v < -127 || v > 127) return ESCAPE_SYM;
     return (v <= 0) ? (-v * 2) - (v != 0) : (v * 2); 
 }
 
 int from_sym(int s) {
     if (s == 0) return 0;
     return (s & 1) ? -((s + 1) / 2) : (s / 2);
 }
 
 void model_init(Model *m) {
     double ctx_exps[CTX_COUNT] = {1.36, 1.28, 1.22, 1.16};
     double ctx_scale[CTX_COUNT] = {45000.0, 35000.0, 25000.0, 20000.0};
 
     for (int c=0; c<CTX_COUNT; c++) {
         m->t[c] = 0;
         for (int i=0; i<MAX_VAL; i++) {
             int v = abs(from_sym(i));
             double d = (double)(v + 1);
             int w = (int)(ctx_scale[c] / pow(d, ctx_exps[c])); 
             if (w < 1) w = 1;
             if (i == ESCAPE_SYM) w = 1; 
             m->f[c][i] = w;
             m->t[c] += w;
         }
         while (m->t[c] >= MAX_TOT) {
             m->t[c] = 0;
             for (int i=0; i<MAX_VAL; i++) {
                 m->f[c][i] = (m->f[c][i] + 1) >> 1;
                 m->t[c] += m->f[c][i];
             }
         }
     }
 }
 
 void model_update(Model *m, int c, int s) {
     m->f[c][s] += INC_MAIN; m->t[c] += INC_MAIN;
     if (s != ESCAPE_SYM) {
         if (s > 0) { m->f[c][s-1] += INC_SIDE; m->t[c] += INC_SIDE; }
         if (s < ESCAPE_SYM - 1) { m->f[c][s+1] += INC_SIDE; m->t[c] += INC_SIDE; }
     }
     if (m->t[c] >= MAX_TOT) {
         m->t[c] = 0;
         for (int i=0; i<MAX_VAL; i++) {
             m->f[c][i] = (m->f[c][i] + 1) >> 1;
             m->t[c] += m->f[c][i];
         }
     }
 }
 
 static inline int quant(int v) { return (int)floor(v/64.0); }
 
 int main(int argc, char **argv) {
     if (argc != 3) {
         fprintf(stderr, "Usage: ./encode input.wav output.bw\n");
         return 1;
     }
 
     samples_t desc;
     short *data = wav_read(argv[1], &desc);
 
     FILE *fh = fopen(argv[2], "wb");
     ASSERT(fh, "Can't open %s for writing", argv[2]);
     
     fwrite_u32_le(desc.samples, fh);
     fwrite_u32_le(desc.samplerate, fh);
     long size_pos = ftell(fh);
     fwrite_u32_le(0, fh);
     
     RangeEnc rc; re_init(&rc);
     Model m; model_init(&m);
 
     int prev_q = 0, prev_mag = 0;
     int d1=0, d2=0, d3=0, d4=0, d5=0, d6=0, d7=0, d8=0;
 
     int w1=700, w2=400, w3=500, w4=400, w5=400, w6=100, w7=250, w8=50;
 
     double avg_mag = 30; 
 
     for (uint32_t i = 0; i < desc.samples; i++) {
         int q = quant(data[i]);
         int d = q - prev_q;
         
         int pred = (d1*w1 + d2*w2 + d3*w3 + d4*w4 + d5*w5 + d6*w6 + d7*w7 + d8*w8) >> P_SHIFT; 
         int res = d + pred; 
 
         int ctx_t1 = (int)(avg_mag * 1);
         int ctx_t2 = (int)(avg_mag * 3);
         int ctx_t3 = (int)(avg_mag * 5);
 
         int ctx;
         if (prev_mag < ctx_t1) ctx = 0;
         else if (prev_mag < ctx_t2) ctx = 1;
         else if (prev_mag < ctx_t3) ctx = 2;
         else ctx = 3;
 
         int sym = to_sym(res);
 
         uint32_t cum = 0;
         for (int j = 0; j < sym; j++) cum += m.f[ctx][j];
         re_encode(&rc, cum, m.f[ctx][sym], m.t[ctx]);
         model_update(&m, ctx, sym);
 
         int rec_res;
         if (sym == ESCAPE_SYM) {
             re_encode(&rc, (res >> 8) & 0xFF, 1, 256);
             re_encode(&rc, res & 0xFF, 1, 256);
             rec_res = res;
         } else {
             rec_res = from_sym(sym);
         }
 
         avg_mag = avg_mag * 0.999995 + abs(rec_res) * 0.000005;
         if (avg_mag < 1.0) avg_mag = 1.0;
 
         int rec_d = rec_res - pred;
         int rec_q = prev_q + rec_d;
         
         int err = rec_res;
         
         if (d1 > 0)      w1 -= (err > 0) ? STEP1 : -STEP1;
         else if (d1 < 0) w1 -= (err > 0) ? -STEP1 : STEP1;
         if (d2 > 0)      w2 -= (err > 0) ? STEP2 : -STEP2;
         else if (d2 < 0) w2 -= (err > 0) ? -STEP2 : STEP2;
         if (d3 > 0)      w3 -= (err > 0) ? STEP3 : -STEP3;
         else if (d3 < 0) w3 -= (err > 0) ? -STEP3 : STEP3;
         if (d4 > 0)      w4 -= (err > 0) ? STEP4 : -STEP4;
         else if (d4 < 0) w4 -= (err > 0) ? -STEP4 : STEP4;
         if (d5 > 0)      w5 -= (err > 0) ? STEP5 : -STEP5;
         else if (d5 < 0) w5 -= (err > 0) ? -STEP5 : STEP5;
         if (d6 > 0)      w6 -= (err > 0) ? STEP6 : -STEP6;
         else if (d6 < 0) w6 -= (err > 0) ? -STEP6 : STEP6;
         if (d7 > 0)      w7 -= (err > 0) ? STEP7 : -STEP7;
         else if (d7 < 0) w7 -= (err > 0) ? -STEP7 : STEP7;
         if (d8 > 0)      w8 -= (err > 0) ? STEP8 : -STEP8;
         else if (d8 < 0) w8 -= (err > 0) ? -STEP8 : STEP8;
 
         if (w1 > 8192) w1 = 8192; if (w1 < -8192) w1 = -8192;
         if (w2 > 8192) w2 = 8192; if (w2 < -8192) w2 = -8192;
         if (w3 > 8192) w3 = 8192; if (w3 < -8192) w3 = -8192;
         if (w4 > 8192) w4 = 8192; if (w4 < -8192) w4 = -8192;
         if (w5 > 8192) w5 = 8192; if (w5 < -8192) w5 = -8192;
         if (w6 > 8192) w6 = 8192; if (w6 < -8192) w6 = -8192;
         if (w7 > 8192) w7 = 8192; if (w7 < -8192) w7 = -8192;
         if (w8 > 8192) w8 = 8192; if (w8 < -8192) w8 = -8192;
 
         prev_q = rec_q; 
         d8=d7; d7=d6; d6=d5; d5=d4; d4=d3; d3=d2; d2=d1; d1=rec_d;
         prev_mag = abs(rec_res);
     }
     re_flush(&rc);
     
     fwrite(rc.buf, 1, rc.buf_size, fh);
     fseek(fh, size_pos, SEEK_SET);
     fwrite_u32_le(rc.buf_size, fh);
     
     free(rc.buf);
     free(data);
     fclose(fh);
     
     return 0;
 }