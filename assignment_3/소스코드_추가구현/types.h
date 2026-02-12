// types.h
#pragma once

// ===== 1) 고정폭 정수 타입 (stdint.h 없이) =====
// xv6 32-bit 전제: unsigned int == 32-bit
typedef unsigned int   uint32_t;   // 32-bit unsigned
typedef int            int32_t;    // 32-bit   signed
typedef unsigned short uint16_t;   // 16-bit unsigned
typedef short          int16_t;    // 16-bit   signed
typedef unsigned char  uint8_t;    // 8-bit  unsigned
typedef signed char    int8_t;     // 8-bit    signed

// ===== 2) 폭 검증 (툴체인이 C11 지원 시) =====
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(uint32_t) == 4, "uint32_t must be 4 bytes");
_Static_assert(sizeof(uint16_t) == 2, "uint16_t must be 2 bytes");
_Static_assert(sizeof(uint8_t)  == 1, "uint8_t must be 1 byte");
#endif

// ===== 3) 32/16/8비트 상수 매크로 (표준 UINT32_C 대체) =====
#define U32_C(x)  x##U    // 이 커널에서 unsigned int는 32-bit 가정
#define U16_C(x)  x##U
#define U8_C(x)   x##U

// ===== 4)  기존 별칭 유지용 브리지 =====
// 기존 코드에 남아있는 uint/ushort/uchar를 잠정 호환하기 위해 선언
typedef uint32_t uint;      
typedef uint16_t ushort;
typedef uint8_t  uchar;

typedef uint pde_t;
