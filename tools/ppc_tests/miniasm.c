// tw2de_nx (2026-11-01): a small PowerPC / VMX128 assembler for Xenia's instruction tests (tools/ppc_tests), built
// on the SDK's own copy of the binutils opcode table (sdk/thirdparty/disasm/ppc-dis.c: names, opcodes, operand insert
// functions), so that no prebuilt assembler is needed. Reads a test .s (after prep.py), writes <out>.bin (the code,
// at 0x82010000) and <out>.map (its labels, nm form). Each word it writes is decoded again with the same table and
// its mnemonic compared (an alias decoding under its other name is reported too: harmless).
//
//   cc -O1 -w -I sdk/thirdparty/disasm miniasm.c sdk/thirdparty/disasm/disasm.c -o miniasm
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppc-dis.c"

#define BASE 0x82010000u
#define DIALECT (0x1 | 0x4 | 0x4000 | 0x8000000 | 0x200 | 0x1000000 | 0x10000)

typedef struct { char name[128]; uint32_t addr; } Label;
static Label labels[131072];
static int nlabels;
static uint32_t words[524288];
static char mns[524288][32];
static int nwords;
static int errors;

static uint32_t find_label(const char *n, int *ok) {
  for (int i = 0; i < nlabels; i++)
    if (!strcmp(labels[i].name, n)) { *ok = 1; return labels[i].addr; }
  *ok = 0;
  return 0;
}

static char *trim(char *s) {
  while (isspace((unsigned char)*s)) s++;
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
  return s;
}

// A register or number operand. Register prefixes: r f v cr (v0..v127). Also "lt/gt/eq/so" not handled.
static int parse_value(const char *t, long *v, int pass, uint32_t pc, int relative) {
  while (isspace((unsigned char)*t)) t++;
  if ((t[0] == 'r' || t[0] == 'f' || t[0] == 'v') && isdigit((unsigned char)t[1])) { *v = strtol(t + 1, 0, 10); return 1; }
  if (t[0] == 'c' && t[1] == 'r' && isdigit((unsigned char)t[2])) { *v = strtol(t + 2, 0, 10); return 1; }
  if (isdigit((unsigned char)t[0]) || t[0] == '-' || t[0] == '+') { *v = strtol(t, 0, 0); return 1; }
  if (!strcmp(t, "xer")) { *v = 1; return 1; }
  if (!strcmp(t, "lr")) { *v = 8; return 1; }
  if (!strcmp(t, "ctr")) { *v = 9; return 1; }
  if (t[0] == '.' && (t[1] == '+' || t[1] == '-')) { long d = strtol(t + 1, 0, 0); *v = relative ? d : (long)(pc + d); return 1; }
  // label
  int ok;
  char n[128];
  snprintf(n, sizeof n, "%s", t);
  char *e = n + strlen(n);
  while (e > n && isspace((unsigned char)e[-1])) *--e = 0;
  uint32_t a = find_label(n, &ok);
  if (!ok) { if (pass == 2) { fprintf(stderr, "unknown label %s\n", n); errors++; } *v = 0; return 1; }
  *v = relative ? (long)(int32_t)(a - pc) : (long)a;
  return 1;
}

static int encode(const char *mn, char ops[][64], int nops, uint32_t pc, int pass, uint32_t *out) {
  for (int k = 0; k < powerpc_num_opcodes; k++) {
    const struct powerpc_opcode *op = &powerpc_opcodes[k];
    if (strcmp(op->name, mn)) continue;
    if (!(op->flags & DIALECT)) continue;
    // count operands (non-fake)
    int total = 0, optional = 0;
    for (const unsigned char *o = op->operands; *o; o++) {
      const struct powerpc_operand *po = &powerpc_operands[*o];
      if (po->flags & PPC_OPERAND_FAKE) continue;
      total++;
      if (po->flags & PPC_OPERAND_OPTIONAL) optional++;
    }
    if (nops > total || nops < total - optional) continue;
    int skip_optional = total - nops;
    unsigned long insn = op->opcode;
    int i = 0, bad = 0;
    for (const unsigned char *o = op->operands; *o; o++) {
      const struct powerpc_operand *po = &powerpc_operands[*o];
      const char *err = 0;
      if (po->flags & PPC_OPERAND_FAKE) {
        insn = po->insert(insn, 0, DIALECT, &err);
        continue;
      }
      if ((po->flags & PPC_OPERAND_OPTIONAL) && skip_optional > 0) {
        skip_optional--;
        if (po->insert) insn = po->insert(insn, 0, DIALECT, &err);
        continue;
      }
      long v;
      parse_value(ops[i++], &v, pass, pc, (po->flags & PPC_OPERAND_RELATIVE) != 0);
      if (po->insert) {
        insn = po->insert(insn, v, DIALECT, &err);
        if (err && pass == 2) bad = 1;
      } else {
        insn |= ((unsigned long)v & po->bitm) << po->shift;
      }
    }
    if (bad) continue;
    *out = (uint32_t)insn;
    return 1;
  }
  return 0;
}

// Split "a, b, 16(r3)" into operands; d(rA) becomes two operands d and rA.
static int split_ops(char *s, char ops[][64]) {
  int n = 0;
  char *p = s;
  while (*p) {
    while (isspace((unsigned char)*p)) p++;
    if (!*p) break;
    char *c = strchr(p, ',');
    char tok[128];
    size_t len = c ? (size_t)(c - p) : strlen(p);
    snprintf(tok, sizeof tok, "%.*s", (int)len, p);
    char *t = trim(tok);
    char *lp = strchr(t, '(');
    if (lp && strchr(lp, ')')) {
      *lp = 0;
      char *rp = strchr(lp + 1, ')');
      *rp = 0;
      snprintf(ops[n++], 64, "%s", *trim(t) ? trim(t) : "0");
      snprintf(ops[n++], 64, "%s", trim(lp + 1));
    } else {
      snprintf(ops[n++], 64, "%s", t);
    }
    if (!c) break;
    p = c + 1;
  }
  return n;
}

static void assemble(FILE *f, int pass) {
  char line[1024];
  uint32_t pc = BASE;
  nwords = 0;
  rewind(f);
  while (fgets(line, sizeof line, f)) {
    char *h = strchr(line, '#');
    if (h) *h = 0;
    char *s = trim(line);
    if (!*s) continue;
    char *colon = strchr(s, ':');
    if (colon && (colon == s || !strchr(s, ' ') || colon < strchr(s, ' '))) {
      *colon = 0;
      if (pass == 1) { snprintf(labels[nlabels].name, 128, "%s", trim(s)); labels[nlabels++].addr = pc; }
      s = trim(colon + 1);
      if (!*s) continue;
    }
    if (*s == '.') {
      if (!strncmp(s, ".long", 5) || !strncmp(s, ".int", 4)) {
        long v = strtol(s + (s[2] == 'o' ? 5 : 4), 0, 0);
        words[nwords++] = (uint32_t)v;
        pc += 4;
      }
      continue;  // other directives ignored
    }
    char mn[64];
    int k = 0;
    while (*s && !isspace((unsigned char)*s) && k < 63) mn[k++] = *s++;
    mn[k] = 0;
    char ops[16][64];
    int nops = split_ops(s, ops);
    uint32_t w = 0;
    if (!encode(mn, ops, nops, pc, pass, &w)) {
      if (pass == 2) { fprintf(stderr, "cannot encode: %s %s\n", mn, s); errors++; }
    }
    snprintf(mns[nwords], 32, "%s", mn);
    words[nwords++] = w;
    pc += 4;
  }
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: miniasm in.s outbase\n"); return 2; }
  FILE *f = fopen(argv[1], "r");
  if (!f) return 2;
  assemble(f, 1);
  assemble(f, 2);
  // verify by disassembling
  disassemble_info info;
  INIT_DISASSEMBLE_INFO(info, stdout, fprintf);
  info.arch = bfd_arch_powerpc;
  info.endian = BFD_ENDIAN_BIG;
  info.private_data = (char *)0 + DIALECT;
  char path[1024];
  snprintf(path, sizeof path, "%s.bin", argv[2]);
  FILE *b = fopen(path, "wb");
  for (int i = 0; i < nwords; i++) {
    uint8_t be[4] = {words[i] >> 24, words[i] >> 16, words[i] >> 8, words[i]};
    fwrite(be, 1, 4, b);
  }
  fclose(b);
  for (int i = 0; i < nwords; i++) {
    if (!mns[i][0]) continue;
    ppc_insn in; memset(&in, 0, sizeof in);
    uint8_t be[4] = {words[i] >> 24, words[i] >> 16, words[i] >> 8, words[i]};
    info.buffer = be; info.buffer_vma = BASE + 4 * i; info.buffer_length = 4;
    decode_insn_ppc(BASE + 4 * i, &info, &in);
    const char *got = in.opcode ? in.opcode->name : "?";
    if (strcmp(got, mns[i])) fprintf(stderr, "%s: %08X %s decodes as %s\n", argv[1], words[i], mns[i], got);
  }
  snprintf(path, sizeof path, "%s.map", argv[2]);
  FILE *m = fopen(path, "w");
  // nm form, addresses relative to the code (rexglue recompile-tests adds 0x82010000)
  for (int i = 0; i < nlabels; i++) fprintf(m, "%08x t %s\n", labels[i].addr - BASE, labels[i].name);
  fclose(m);
  if (errors) fprintf(stderr, "%s: %d errors\n", argv[1], errors);
  return errors ? 1 : 0;
}
