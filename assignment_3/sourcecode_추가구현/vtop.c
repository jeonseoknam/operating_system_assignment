// vtop.c
#include "types.h"
#include "stat.h"
#include "user.h"

// 공백 판별
static int is_space(char c) {
  return c==' ' || c=='\t' || c=='\n' || c=='\r' || c=='\v' || c=='\f';
}

// 입력 전체에 A–F/a–f가 하나라도 있으면 16진으로 간주
static int has_hex_alpha(const char *s) {
  for (; *s; s++) {
    if (*s=='x' || *s=='X') continue;
    if ((*s>='a' && *s<='f') || (*s>='A' && *s<='F')) return 1;
  }
  return 0;
}

// 자동 진수 판별 파서: 0x... → 16진, 그 외에 A–F/a–f 포함 → 16진, 아니면 10진
static int parse_u32_autobase(const char *s, uint *out) {
  int base = 10;
  uint val = 0;
  int i = 0, n_digits = 0;

  while (is_space(s[i])) i++;

  if (s[i]=='0' && (s[i+1]=='x' || s[i+1]=='X')) { base = 16; i += 2; }
  else if (has_hex_alpha(s + i)) { base = 16; }

  for (; s[i]; i++) {
    char c = s[i];
    if (is_space(c)) break;
    int d = -1;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return -1; // 유효하지 않은 문자
    if (d >= base) return -1;
    val = val * base + (uint)d;
    n_digits++;
  }

  while (is_space(s[i])) i++;
  if (s[i] != 0) return -1;     // 뒷부분에 쓰레기 문자가 남음
  if (n_digits == 0) return -1; // 숫자를 하나도 못 읽음

  *out = val;
  return 0;
}

int
main(int argc, char *argv[])
{
  if (argc != 2) {
    printf(1, "usage: vtop <va>\n");
    printf(1, "  accepts: decimal (e.g., 36864), hex with 0x (e.g., 0x9000), or bare hex (e.g., 9FFF)\n");
    exit();
  }

  uint va_u = 0;
  if (parse_u32_autobase(argv[1], &va_u) < 0) {
    printf(1, "vtop: invalid address '%s'\n", argv[1]);
    exit();
  }

  void *va = (void*)va_u;
  uint pa = 0, flags = 0;
  int rc = vtop(va, &pa, &flags);
  if (rc == 0)
    printf(1, "VA=0x%x -> PA=0x%x flags=0x%x\n", va_u, pa, flags);
  else
    printf(1, "vtop failed rc=%d\n", rc);
  exit();
}

