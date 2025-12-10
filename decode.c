 #include <stdio.h>
 #include <stdlib.h>
 #include <math.h>
 #include <string.h>
 #include <stdint.h>
 
 #define ABORT(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } while(0)
 #define ASSERT(TEST, ...) do { if (!(TEST)) ABORT(__VA_ARGS__); } while(0)
 
 #define MAX_TOT    200000 
 #define INC_MAIN   90    
 #define INC_SIDE   20    
 #define P_SHIFT    12
 
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
 
 void fwrite_u32_le(uint32_t v, FILE *fh) {
     uint8_t buf[4] = {v, v>>8, v>>16, v>>24};
     fwrite(buf, 4, 1, fh);
 }
 
 void fwrite_u16_le(unsigned short v, FILE *fh) {
     uint8_t buf[2] = {v, v>>8};
     fwrite(buf, 2, 1, fh);
 }
 
 uint32_t fread_u32_le(FILE *fh) {
     uint8_t buf[4];
     if(fread(buf, 4, 1, fh) != 1) return 0;
     return (buf[3]<<24) | (buf[2]<<16) | (buf[1]<<8) | buf[0];
 }
 
 int wav_write(const char *path, short *data, samples_t *desc) {
     uint32_t data_sz = desc->samples * desc->channels * sizeof(short);
     FILE *fh = fopen(path, "wb");
     ASSERT(fh, "Can't open %s", path);
     fwrite("RIFF", 1, 4, fh); fwrite_u32_le(data_sz + 36, fh);
     fwrite("WAVEfmt \x10\x00\x00\x00\x01\x00", 1, 14, fh);
     fwrite_u16_le(desc->channels, fh); fwrite_u32_le(desc->samplerate, fh);
     fwrite_u32_le(desc->samplerate*2*desc->channels, fh); 
     fwrite_u16_le(desc->channels*2, fh); fwrite_u16_le(16, fh);
     fwrite("data", 1, 4, fh); fwrite_u32_le(data_sz, fh);
     fwrite(data, 1, data_sz, fh);
     fclose(fh);
     return data_sz + 44;
 }
 
 typedef struct {
     const uint8_t *buf;
     uint32_t buf_size;
     uint32_t pos;
     uint32_t range;
     uint32_t code;
 } RangeDec;
 
 uint8_t rd_in_byte(RangeDec *rd) {
     if (rd->pos < rd->buf_size) return rd->buf[rd->pos++];
     return 0;
 }
 
 void rd_init(RangeDec *rd, const uint8_t *buf, uint32_t size) {
     rd->buf = buf; rd->buf_size = size; rd->pos = 0;
     rd->code = 0; rd->range = 0xFFFFFFFF;
     for (int i = 0; i < 5; i++) rd->code = (rd->code << 8) | rd_in_byte(rd);
 }
 
 void rd_normalize(RangeDec *rd) {
     while (rd->range < (1u << 24)) {
         rd->code = (rd->code << 8) | rd_in_byte(rd);
         rd->range <<= 8;
     }
 }
 
 uint32_t rd_get_freq(RangeDec *rd, uint32_t tot) {
     rd->range /= tot;
     return rd->code / rd->range;
 }
 
 void rd_update(RangeDec *rd, uint32_t cum, uint32_t freq) {
     rd->code -= cum * rd->range;
     rd->range *= freq;
     rd_normalize(rd);
 }
 
 #define MAX_VAL 256
 #define ESCAPE_SYM 255
 #define CTX_COUNT 4
 
 typedef struct {
     uint32_t f[CTX_COUNT][MAX_VAL];
     uint32_t t[CTX_COUNT];
 } Model;
 
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
 
 static inline int dequant(int v) {
     if (v >= 0) return round(v * 64.061577 + 31.034184);
     return -round((-v - 1) * 64.061577 + 31.034184) - 1;
 }
 
 int main(int argc, char **argv) {
     if (argc != 3) {
         fprintf(stderr, "Usage: ./decode input.bw output.wav\n");
         return 1;
     }
 
     FILE *fh = fopen(argv[1], "rb"); 
     ASSERT(fh, "Can't open %s", argv[1]);
     
     samples_t desc;
     desc.samples = fread_u32_le(fh);
     desc.samplerate = fread_u32_le(fh);
     desc.channels = 1;
     uint32_t comp_size = fread_u32_le(fh);
     
     uint8_t *comp_buf = malloc(comp_size + 32);
     fread(comp_buf, 1, comp_size, fh);
     memset(comp_buf + comp_size, 0, 32);
     fclose(fh);
 
     RangeDec rd; 
     rd_init(&rd, comp_buf, comp_size + 32);
     Model m; model_init(&m);
 
     short *data = malloc(desc.samples * sizeof(short));
     int prev_q = 0, prev_mag = 0;
     int d1=0, d2=0, d3=0, d4=0, d5=0, d6=0, d7=0, d8=0;
 
     int w1=700, w2=400, w3=500, w4=400, w5=400, w6=100, w7=250, w8=50;
 
     double avg_mag = 30; 
 
     for (uint32_t i = 0; i < desc.samples; i++) {
         int ctx_t1 = (int)(avg_mag * 1);
         int ctx_t2 = (int)(avg_mag * 3);
         int ctx_t3 = (int)(avg_mag * 5);
 
         int ctx;
         if (prev_mag < ctx_t1) ctx = 0;
         else if (prev_mag < ctx_t2) ctx = 1;
         else if (prev_mag < ctx_t3) ctx = 2;
         else ctx = 3;
         
         uint32_t val = rd_get_freq(&rd, m.t[ctx]);
         uint32_t cum = 0;
         int sym = 0;
         for (int j = 0; j < MAX_VAL; j++) {
             if (val < cum + m.f[ctx][j]) { sym = j; break; }
             cum += m.f[ctx][j];
         }
         rd_update(&rd, cum, m.f[ctx][sym]);
         model_update(&m, ctx, sym);
 
         int res;
         if (sym == ESCAPE_SYM) {
             int b1 = rd_get_freq(&rd, 256);
             rd_update(&rd, b1, 1);
             int b2 = rd_get_freq(&rd, 256);
             rd_update(&rd, b2, 1);
             res = (int16_t)((b1 << 8) | b2);
         } else {
             res = from_sym(sym);
         }
 
         int pred = (d1*w1 + d2*w2 + d3*w3 + d4*w4 + d5*w5 + d6*w6 + d7*w7 + d8*w8) >> P_SHIFT;
         
         int d = res - pred; 
         int q = prev_q + d;
         
         avg_mag = avg_mag * 0.999995 + abs(res) * 0.000005;
         if (avg_mag < 1.0) avg_mag = 1.0;
 
         int err = res;
         
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
 
         data[i] = dequant(q);
         prev_q = q; 
         d8=d7; d7=d6; d6=d5; d5=d4; d4=d3; d3=d2; d2=d1; d1=d;
         prev_mag = abs(res);
     }
     
     wav_write(argv[2], data, &desc);
     
     free(comp_buf);
     free(data);
     return 0;
 }