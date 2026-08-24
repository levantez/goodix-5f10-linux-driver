// Проверка порта генерации DEVICE_CONFIG: из НАШЕГО OTP собрать конфиг и сравнить
// байт-в-байт с рабочим эталоном (config_milang.hex). Без устройства.
//   gcc tools/milang_verify.c -Isrc -o /tmp/milang_verify && /tmp/milang_verify
#include "milang_config.h"
#include <stdio.h>

// OTP нашего устройства (из docs/calibration.md)
static const uint8_t our_otp[32] = {
  0xea, 0x46, 0x86, 0x66, 0xd4, 0x50, 0x1c, 0xfc,
  0x30, 0xaf, 0x00, 0x71, 0x02, 0x04, 0x46, 0x74,
  0x2e, 0x20, 0xa2, 0x69, 0x01, 0x00, 0xf9, 0x06,
  0x9e, 0xf9, 0x36, 0xc9, 0xc0, 0x9d, 0x34, 0x36,
};

int
main (void)
{
  uint8_t out[GX5F10_CONFIG_LEN];
  gx5f10_build_config (our_otp, out);
  for (int i = 0; i < GX5F10_CONFIG_LEN; i++)
    printf ("%02x", out[i]);
  printf ("\n");
  return 0;
}
