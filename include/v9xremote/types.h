#ifndef V9XREMOTE_TYPES_H
#define V9XREMOTE_TYPES_H

/* Project code targets 16-bit and 32-bit x86 Open Watcom data models. */
typedef unsigned char  v9x_u8;
typedef unsigned short v9x_u16;
typedef unsigned long  v9x_u32;
typedef signed short   v9x_s16;
typedef signed long    v9x_s32;

typedef char v9x_assert_u8_is_1[(sizeof(v9x_u8) == 1) ? 1 : -1];
typedef char v9x_assert_u16_is_2[(sizeof(v9x_u16) == 2) ? 1 : -1];
typedef char v9x_assert_u32_is_4[(sizeof(v9x_u32) == 4) ? 1 : -1];

#define V9X_FALSE ((v9x_u16)0u)
#define V9X_TRUE  ((v9x_u16)1u)

#endif
