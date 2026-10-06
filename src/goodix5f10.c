// Goodix TLS driver for libfprint — модель 27c6:5f10 (milanG, HONOR), SIGFM-матчинг.
//
// НЕ image-устройство (NBIS даёт 2 минуции на этом сенсоре — доказано). Это кастомный
// FpDevice: сами захватываем кадр (транспорт goodix.c + TLS goodixtls.c), извлекаем SIFT
// (sigfm), храним шаблон из нескольких кадров в FpPrint (fpi-data), матчим sigfm_match_score.
// Системный libfprint не трогаем. Основано на goodixtls (LGPL-2.1+).

#include "fp-device.h"
#include "fpi-device.h"
#include "fpi-print.h"
#include "fpi-ssm.h"

#define FP_COMPONENT "goodixtls5f10"

#include <gmodule.h>
#include <glib.h>
#include <string.h>
#include <stdlib.h>

#include <openssl/ssl.h>

#include "drivers_api.h"
#include "goodix.h"
#include "goodix_proto.h"
#include "goodixtls.h"
#include "goodix5f10.h"
#include "milang_config.h"
#include "sigfm/sigfm.h"

// ---- геометрия / параметры детекта (см. диагностику) ----
#define WIDTH  56
#define HEIGHT 176
#define FRAME  (WIDTH * HEIGHT)
#define FDT_LEN 24
#define FDT_HDR 4
#define ZONES   10
#define FINGER_DROP   0x30
#define FINGER_ZONES  7   // полное касание
#define FINGER_OFF    3
#define POLL_MS       30
#define SETTLE_MS     150
#define VERIFY_MS     500   // verify/identify: окно перезахвата от касания, пока нет совпадения
#define RECAP_MS      20    // пауза между перезахватами внутри окна
#define SIGFM_THRESHOLD 20   // из бенчмарка: свой(перекрытие) >>1000, чужой=0
#define ENROLL_STAGES   16   // шаблон из нескольких кадров (покрытие пальца)
#define MIN_KEYPOINTS   30   // отбраковка слабых кадров при enroll

typedef guint16 Pix;

struct _FpiDeviceGoodixTls5f10
{
  FpiDeviceGoodixTls parent;

  Pix      *calib;             // фоновый кадр (без пальца)
  Pix      *finger;            // последний кадр пальца (raw 16-бит)
  guint8    fdt_base[FDT_LEN];
  guint16   baseline[ZONES];
  gboolean  have_base;

  guint8    gen_config[GX5F10_CONFIG_LEN];  // DEVICE_CONFIG, сгенерённый из OTP этого устройства
  gboolean  have_config;

  GPtrArray *enroll_infos;     // SigfmImgInfo* собранные при enroll
  int        enroll_target;
  FpPrint   *enroll_print;

  gint64     cap_deadline;     // до какого момента перезахватывать (verify/identify)
  gboolean   cap_matched;      // вердикт последнего кадра
  FpPrint   *cap_match_print;  // кто совпал при identify (ссылка не наша)
};

G_DECLARE_FINAL_TYPE (FpiDeviceGoodixTls5f10, fpi_device_goodixtls5f10, FPI,
                      DEVICE_GOODIXTLS5F10, FpiDeviceGoodixTls);
G_DEFINE_TYPE (FpiDeviceGoodixTls5f10, fpi_device_goodixtls5f10,
               FPI_TYPE_DEVICE_GOODIXTLS);

// ===================== image helpers =====================

static void
decode_frame (Pix *out, guint32 len, const guint8 *raw)
{
  Pix *p = out;
  for (guint32 i = 8; i + 6 <= len - 5 + 6 && i < len - 5; i += 6)
    {
      const guint8 *c = raw + i;
      *p++ = ((c[0] & 0xf) << 8) + c[1];
      *p++ = (c[3] << 4) + (c[0] >> 4);
      *p++ = ((c[5] & 0xf) << 8) + c[2];
      *p++ = (c[4] << 4) + (c[5] >> 4);
    }
}

static void
subtract_bg (Pix *src, const Pix *bg, int n)
{
  const guint16 mx = 0xffff;
  for (int i = 0; i < n; i++)
    src[i] = MAX (0, mx - ((mx - src[i]) - (mx - bg[i])));
}

// перцентильная нормализация (клип 2/98) -> 8 бит
static void
squash (const Pix *frame, guint8 *out, int n)
{
  guint32 hist[4096] = { 0 };
  for (int i = 0; i < n; i++)
    hist[frame[i] >> 4]++;
  guint32 lo_t = (guint32) n * 2 / 100, hi_t = (guint32) n * 98 / 100, acc = 0;
  int lo = 0, hi = 65535;
  for (int v = 0; v < 4096; v++) { acc += hist[v]; if (acc >= lo_t) { lo = v << 4; break; } }
  acc = 0;
  for (int v = 0; v < 4096; v++) { acc += hist[v]; if (acc >= hi_t) { hi = v << 4; break; } }
  if (hi <= lo) hi = lo + 1;
  for (int i = 0; i < n; i++)
    {
      int p = frame[i]; if (p < lo) p = lo; if (p > hi) p = hi;
      out[i] = (p - lo) * 255 / (hi - lo);
    }
}

static void
gen_fdt_base (const guint8 *reply, guint8 *base)
{
  for (int i = 0; i < FDT_HDR; i++) base[i] = 0xFF;
  for (int i = FDT_HDR; i < FDT_LEN; i += 2)
    {
      guint16 v = reply[i] | ((guint16) reply[i + 1] << 8);
      guint32 b = (((guint32) (v & 0xFFFE)) << 7) | (v >> 1);
      base[i] = b & 0xFF; base[i + 1] = (b >> 8) & 0xFF;
    }
}

static int
finger_zones (const guint8 *reply, guint16 len, const guint16 *base)
{
  int c = 0;
  if (len < FDT_LEN) return 0;
  for (int z = 0; z < ZONES; z++)
    {
      int off = FDT_HDR + z * 2;
      int cur = reply[off] | ((int) reply[off + 1] << 8);
      if ((int) base[z] - cur >= FINGER_DROP) c++;
    }
  return c;
}

// ===================== capture SSM =====================
// Захват: ждём отрыв (с прошлого касания) -> query_mcu -> fdt_mode(база) -> калибровка(фон) ->
// ждём палец -> снимок (verify/identify: перезахват в окне VERIFY_MS). Затем g_capture_cb.

typedef void (*CaptureCb)(FpDevice *dev, GError *err);

// CAP_WAIT_OFF первым: результат отдаём сразу после кадра, а снятие пальца
// ждём уже в начале следующего захвата (иначе одно касание засчитается дважды
// и фон в CAP_FDT_MODE/CAP_CALIBRATE снимется с пальцем)
enum cap_states {
  CAP_WAIT_OFF, CAP_QUERY_MCU, CAP_FDT_MODE, CAP_CALIBRATE,
  CAP_WAIT_ON, CAP_CAPTURE, CAP_NUM,
};

// verify/identify: решение по свежему кадру; FALSE — перезахват, пока не вышло окно.
// У enroll NULL: там каждый кадр идёт в шаблон.
typedef gboolean (*DecideCb)(FpDevice *dev);

static CaptureCb g_capture_cb;  // одна операция за раз
static DecideCb  g_decide_cb;

static const guint8 fdt_payload[] = {
  0x01, 0x80, 0xb1, 0x80, 0xc1, 0x80, 0xa6, 0x80, 0xb6, 0x80, 0xa5, 0x80, 0xb6,
};

static void
cap_query_cb (FpDevice *dev, guchar *st, guint16 len, gpointer ssm, GError *e)
{ if (e) fpi_ssm_mark_failed (ssm, e); else fpi_ssm_next_state (ssm); }

static void
cap_fdt_mode_cb (FpDevice *dev, guint8 *data, guint16 len, gpointer ssm, GError *e)
{
  if (e) { fpi_ssm_mark_failed (ssm, e); return; }
  FpiDeviceGoodixTls5f10 *self = FPI_DEVICE_GOODIXTLS5F10 (dev);
  if (len >= FDT_LEN)
    {
      gen_fdt_base (data, self->fdt_base);
      for (int z = 0; z < ZONES; z++)
        {
          int off = FDT_HDR + z * 2;
          self->baseline[z] = data[off] | ((guint16) data[off + 1] << 8);
        }
      self->have_base = TRUE;
    }
  fpi_ssm_next_state (ssm);
}

static void
cap_calib_cb (FpDevice *dev, guint8 *data, guint16 len, gpointer ssm, GError *e)
{
  if (e) { fpi_ssm_mark_failed (ssm, e); return; }
  FpiDeviceGoodixTls5f10 *self = FPI_DEVICE_GOODIXTLS5F10 (dev);
  if (!self->calib) self->calib = calloc (FRAME, sizeof (Pix));
  decode_frame (self->calib, len, data);
  fpi_ssm_next_state (ssm);
}

static void
cap_wait_on_cb (FpDevice *dev, guint8 *data, guint16 len, gpointer ssm, GError *e)
{
  if (e) { fpi_ssm_mark_failed (ssm, e); return; }
  FpiDeviceGoodixTls5f10 *self = FPI_DEVICE_GOODIXTLS5F10 (dev);
  int zc = finger_zones (data, len, self->baseline);
  if (zc >= FINGER_ZONES)
    {
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_PRESENT);
      self->cap_deadline = g_get_monotonic_time () + VERIFY_MS * 1000;
      fpi_ssm_next_state_delayed (ssm, SETTLE_MS);
    }
  else
    fpi_ssm_jump_to_state_delayed (ssm, CAP_WAIT_ON, POLL_MS);
}

static void
cap_capture_cb (FpDevice *dev, guint8 *data, guint16 len, gpointer ssm, GError *e)
{
  if (e) { fpi_ssm_mark_failed (ssm, e); return; }
  FpiDeviceGoodixTls5f10 *self = FPI_DEVICE_GOODIXTLS5F10 (dev);
  if (!self->finger) self->finger = calloc (FRAME, sizeof (Pix));
  decode_frame (self->finger, len, data);
  // неудачное прилегание даёт мало точек — пока палец лежит, пробуем ещё кадр
  if (g_decide_cb && !g_decide_cb (dev) && g_get_monotonic_time () < self->cap_deadline)
    {
      fpi_ssm_jump_to_state_delayed (ssm, CAP_CAPTURE, RECAP_MS);
      return;
    }
  fpi_ssm_next_state (ssm);
}

static void
cap_wait_off_cb (FpDevice *dev, guint8 *data, guint16 len, gpointer ssm, GError *e)
{
  if (e) { fpi_ssm_mark_failed (ssm, e); return; }
  FpiDeviceGoodixTls5f10 *self = FPI_DEVICE_GOODIXTLS5F10 (dev);
  int zc = finger_zones (data, len, self->baseline);
  if (zc < FINGER_OFF)
    {
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);
      fpi_ssm_next_state (ssm);
    }
  else
    fpi_ssm_jump_to_state_delayed (ssm, CAP_WAIT_OFF, POLL_MS);
}

static void
cap_run (FpiSsm *ssm, FpDevice *dev)
{
  guint8 payload[1 + FDT_LEN];
  FpiDeviceGoodixTls5f10 *self = FPI_DEVICE_GOODIXTLS5F10 (dev);

  // fprintd отменяет verify (VerifyStop, таймаут pam_fprintd, смерть клиента) —
  // без этой проверки опрос CAP_WAIT_ON крутится вечно: claim не снимается,
  // libfprint копит "нагрев" и блокирует устройство (overheating)
  if (fpi_device_action_is_cancelled (dev))
    {
      fpi_ssm_mark_failed (ssm, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                     "операция отменена"));
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAP_QUERY_MCU:
      goodix_send_query_mcu_state (dev, cap_query_cb, ssm);
      break;
    case CAP_FDT_MODE:
      goodix_send_mcu_switch_to_fdt_mode (dev, fdt_payload, sizeof (fdt_payload),
                                          NULL, cap_fdt_mode_cb, ssm);
      break;
    case CAP_CALIBRATE:
      goodix_tls_read_image (dev, cap_calib_cb, ssm);
      break;
    case CAP_WAIT_ON:
      payload[0] = 0x01;
      if (self->have_base) memcpy (payload + 1, self->fdt_base, FDT_LEN);
      else memset (payload + 1, 0xFF, FDT_LEN);
      goodix_send_mcu_switch_to_fdt_down (dev, payload, sizeof (payload),
                                          NULL, cap_wait_on_cb, ssm);
      break;
    case CAP_CAPTURE:
      goodix_tls_read_image (dev, cap_capture_cb, ssm);
      break;
    case CAP_WAIT_OFF:
      // первый захват: базовой линии ещё нет, палец снимать не с чего
      if (!self->have_base) { fpi_ssm_next_state (ssm); break; }
      payload[0] = 0x01;
      if (self->have_base) memcpy (payload + 1, self->fdt_base, FDT_LEN);
      else memset (payload + 1, 0xFF, FDT_LEN);
      goodix_send_mcu_switch_to_fdt_down (dev, payload, sizeof (payload),
                                          NULL, cap_wait_off_cb, ssm);
      break;
    }
}

static void
cap_complete (FpiSsm *ssm, FpDevice *dev, GError *err)
{
  CaptureCb cb = g_capture_cb;
  g_capture_cb = NULL;
  g_decide_cb = NULL;
  if (cb) cb (dev, err);
  else if (err) g_error_free (err);
}

// снять один кадр -> cb(dev, err); при успехе self->finger валиден
static void
start_capture (FpDevice *dev, CaptureCb cb, DecideCb decide)
{
  FpiDeviceGoodixTls5f10 *self = FPI_DEVICE_GOODIXTLS5F10 (dev);
  g_capture_cb = cb;
  g_decide_cb = decide;
  self->cap_matched = FALSE;
  self->cap_match_print = NULL;
  fpi_ssm_start (fpi_ssm_new (dev, cap_run, CAP_NUM), cap_complete);
}

// извлечь SIGFM из последнего кадра пальца (фон вычтен, нормализовано)
static SigfmImgInfo *
extract_last (FpiDeviceGoodixTls5f10 *self)
{
  if (!self->finger) return NULL;
  Pix *raw = calloc (FRAME, sizeof (Pix));
  memcpy (raw, self->finger, FRAME * sizeof (Pix));
  if (self->calib) subtract_bg (raw, self->calib, FRAME);
  guint8 *img = calloc (FRAME, 1);
  squash (raw, img, FRAME);
  free (raw);
  SigfmImgInfo *info = sigfm_extract (img, WIDTH, HEIGHT);
  free (img);
  return info;
}

// ===================== template storage (FpPrint <-> sigfm) =====================

static void
store_template (FpPrint *print, GPtrArray *infos)
{
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE ("aay"));
  for (guint i = 0; i < infos->len; i++)
    {
      int len = 0;
      unsigned char *ser = sigfm_serialize_binary (g_ptr_array_index (infos, i), &len);
      if (!ser || len <= 0) { free (ser); continue; }
      g_variant_builder_add_value (&b,
        g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, ser, len, 1));
      free (ser);
    }
  GVariant *data = g_variant_builder_end (&b);
  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, FALSE);
  g_object_set (print, "fpi-data", data, NULL);
}

// вернуть массив SigfmImgInfo* из FpPrint (вызывающий освобождает через sigfm_free_info)
static GPtrArray *
load_template (FpPrint *print)
{
  GVariant *data = NULL;
  g_object_get (print, "fpi-data", &data, NULL);
  if (!data) return NULL;
  GPtrArray *infos = g_ptr_array_new ();
  GVariantIter it;
  GVariant *child;
  g_variant_iter_init (&it, data);
  while ((child = g_variant_iter_next_value (&it)))
    {
      gsize n = 0;
      const guchar *bytes = g_variant_get_fixed_array (child, &n, 1);
      SigfmImgInfo *info = sigfm_deserialize_binary (bytes, (int) n);
      if (info) g_ptr_array_add (infos, info);
      g_variant_unref (child);
    }
  g_variant_unref (data);
  return infos;
}

static gboolean
match_infos (GPtrArray *tmpl, SigfmImgInfo *probe)
{
  for (guint i = 0; i < tmpl->len; i++)
    {
      int s = sigfm_match_score (g_ptr_array_index (tmpl, i), probe);
      fp_dbg ("sigfm score %d/%d (шаблон %u)", s, SIGFM_THRESHOLD, i);
      if (s >= SIGFM_THRESHOLD) return TRUE;
    }
  return FALSE;
}

// ===================== activate / open / close =====================

enum act_states {
  ACT_NOP0, ACT_ENABLE, ACT_NOP1, ACT_RESET, ACT_READ_OTP, ACT_CONFIG, ACT_PWR, ACT_NUM,
};

// OTP этого устройства -> генерим DEVICE_CONFIG (DAC под конкретный сенсор)
static void
act_read_otp_cb (FpDevice *dev, guint8 *data, guint16 len, gpointer ssm, GError *e)
{
  if (e) { fpi_ssm_mark_failed (ssm, e); return; }
  FpiDeviceGoodixTls5f10 *self = FPI_DEVICE_GOODIXTLS5F10 (dev);
  if (len < 32)
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                           "OTP слишком короткий: %u", len));
      return;
    }
  gx5f10_build_config (data, self->gen_config);
  self->have_config = TRUE;
  fp_dbg ("DEVICE_CONFIG сгенерирован из OTP (%u байт OTP)", len);
  fpi_ssm_next_state (ssm);
}

static void
act_check_none (FpDevice *dev, gpointer ssm, GError *e)
{ if (e) fpi_ssm_mark_failed (ssm, e); else fpi_ssm_next_state (ssm); }

static void
act_check_reset (FpDevice *dev, gboolean ok, guint16 num, gpointer ssm, GError *e)
{ if (e) fpi_ssm_mark_failed (ssm, e); else fpi_ssm_next_state (ssm); }

static void
act_check_success (FpDevice *dev, gboolean ok, gpointer ssm, GError *e)
{
  if (e) fpi_ssm_mark_failed (ssm, e);
  else if (!ok) fpi_ssm_mark_failed (ssm,
        fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "команда не удалась"));
  else fpi_ssm_next_state (ssm);
}

static void
act_run (FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ACT_NOP0:
      goodix_start_read_loop (dev);
      goodix_send_nop (dev, act_check_none, ssm);
      break;
    case ACT_ENABLE:
      goodix_send_enable_chip (dev, TRUE, act_check_none, ssm);
      break;
    case ACT_NOP1:
      goodix_send_nop (dev, act_check_none, ssm);
      break;
    case ACT_RESET:
      goodix_send_reset (dev, TRUE, 20, act_check_reset, ssm);
      break;
    case ACT_READ_OTP:
      goodix_send_read_otp (dev, act_read_otp_cb, ssm);
      break;
    case ACT_CONFIG:
      {
        // конфиг, сгенерированный из OTP этого устройства (fallback — статический)
        FpiDeviceGoodixTls5f10 *self = FPI_DEVICE_GOODIXTLS5F10 (dev);
        guint8 *cfg = self->have_config ? self->gen_config : goodix_5f10_config;
        goodix_send_upload_config_mcu (dev, cfg, GX5F10_CONFIG_LEN, NULL,
                                       act_check_success, ssm);
      }
      break;
    case ACT_PWR:
      goodix_send_set_powerdown_scan_frequency (dev, 100, act_check_success, ssm);
      break;
    }
}

static void
tls_ready (FpDevice *dev, gpointer user_data, GError *err)
{
  fpi_device_open_complete (dev, err);
}

static void
act_complete (FpiSsm *ssm, FpDevice *dev, GError *err)
{
  if (err) { fpi_device_open_complete (dev, err); return; }
  goodix_tls_init (dev, tls_ready, NULL);
}

static void
dev_open (FpDevice *dev)
{
  GError *err = NULL;
  // goodix_dev_init возвращает TRUE на УСПЕХ (claim_interface); на провале — FALSE+err
  if (!goodix_dev_init (dev, &err)) { fpi_device_open_complete (dev, err); return; }
  fpi_ssm_start (fpi_ssm_new (dev, act_run, ACT_NUM), act_complete);
}

static void
dev_close (FpDevice *dev)
{
  GError *err = NULL;
  // goodix_dev_deinit сам делает shutdown_tls + reset_state — не дублируем (иначе double-free)
  goodix_dev_deinit (dev, &err);
  fpi_device_close_complete (dev, err);
}

static void
dev_probe (FpDevice *dev)
{
  fpi_device_probe_complete (dev, "goodixtls5f10",
                             "Goodix 5f10 (milanG, SIGFM)", NULL);
}

// ===================== enroll =====================

static void enroll_capture_cb (FpDevice *dev, GError *err);

static void
enroll_next (FpDevice *dev)
{
  start_capture (dev, enroll_capture_cb, NULL);
}

static void
enroll_capture_cb (FpDevice *dev, GError *err)
{
  FpiDeviceGoodixTls5f10 *self = FPI_DEVICE_GOODIXTLS5F10 (dev);
  if (err) { fpi_device_enroll_complete (dev, NULL, err); return; }
  if (fpi_device_action_is_cancelled (dev))
    {
      fpi_device_enroll_complete (dev, NULL,
        fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }

  SigfmImgInfo *info = extract_last (self);
  int kp = info ? sigfm_keypoints_count (info) : 0;
  if (info && kp >= MIN_KEYPOINTS)
    {
      g_ptr_array_add (self->enroll_infos, info);
      fpi_device_enroll_progress (dev, self->enroll_infos->len, NULL, NULL);
    }
  else
    {
      if (info) sigfm_free_info (info);
      // плохой кадр — просим ещё, прогресс не двигаем
      fpi_device_enroll_progress (dev, self->enroll_infos->len, NULL,
        fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
    }

  if ((int) self->enroll_infos->len >= self->enroll_target)
    {
      store_template (self->enroll_print, self->enroll_infos);
      FpPrint *done = self->enroll_print;
      self->enroll_print = NULL;
      fpi_device_enroll_complete (dev, g_object_ref (done), NULL);
    }
  else
    enroll_next (dev);
}

static void
dev_enroll (FpDevice *dev)
{
  FpiDeviceGoodixTls5f10 *self = FPI_DEVICE_GOODIXTLS5F10 (dev);
  FpPrint *print = NULL;
  fpi_device_get_enroll_data (dev, &print);
  self->enroll_print = print;
  self->enroll_target = ENROLL_STAGES;
  if (self->enroll_infos) g_ptr_array_free (self->enroll_infos, TRUE);
  self->enroll_infos = g_ptr_array_new_with_free_func ((GDestroyNotify) sigfm_free_info);
  enroll_next (dev);
}

// ===================== verify / identify =====================

static gboolean
verify_decide (FpDevice *dev)
{
  FpiDeviceGoodixTls5f10 *self = FPI_DEVICE_GOODIXTLS5F10 (dev);
  FpPrint *tmpl = NULL;
  fpi_device_get_verify_data (dev, &tmpl);
  GPtrArray *infos = load_template (tmpl);
  SigfmImgInfo *probe = extract_last (self);

  gboolean match = (infos && probe) ? match_infos (infos, probe) : FALSE;
  if (probe) sigfm_free_info (probe);
  if (infos) g_ptr_array_foreach (infos, (GFunc) sigfm_free_info, NULL),
             g_ptr_array_free (infos, TRUE);

  self->cap_matched = match;
  return match;
}

static void
verify_capture_cb (FpDevice *dev, GError *err)
{
  FpiDeviceGoodixTls5f10 *self = FPI_DEVICE_GOODIXTLS5F10 (dev);
  if (err) { fpi_device_verify_complete (dev, err); return; }

  fpi_device_verify_report (dev,
    self->cap_matched ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL, NULL, NULL);
  fpi_device_verify_complete (dev, NULL);
}

static void
dev_verify (FpDevice *dev)
{
  start_capture (dev, verify_capture_cb, verify_decide);
}

static gboolean
identify_decide (FpDevice *dev)
{
  FpiDeviceGoodixTls5f10 *self = FPI_DEVICE_GOODIXTLS5F10 (dev);
  GPtrArray *gallery = NULL;
  fpi_device_get_identify_data (dev, &gallery);
  SigfmImgInfo *probe = extract_last (self);
  FpPrint *found = NULL;

  for (guint i = 0; probe && gallery && i < gallery->len; i++)
    {
      FpPrint *cand = g_ptr_array_index (gallery, i);
      GPtrArray *infos = load_template (cand);
      gboolean m = infos ? match_infos (infos, probe) : FALSE;
      if (infos) g_ptr_array_foreach (infos, (GFunc) sigfm_free_info, NULL),
                 g_ptr_array_free (infos, TRUE);
      if (m) { found = cand; break; }
    }
  if (probe) sigfm_free_info (probe);

  self->cap_match_print = found;
  self->cap_matched = (found != NULL);
  return self->cap_matched;
}

static void
identify_capture_cb (FpDevice *dev, GError *err)
{
  FpiDeviceGoodixTls5f10 *self = FPI_DEVICE_GOODIXTLS5F10 (dev);
  if (err) { fpi_device_identify_complete (dev, err); return; }

  fpi_device_identify_report (dev, self->cap_match_print, NULL, NULL);
  fpi_device_identify_complete (dev, NULL);
}

static void
dev_identify (FpDevice *dev)
{
  start_capture (dev, identify_capture_cb, identify_decide);
}

// ===================== type =====================

static void
fpi_device_goodixtls5f10_init (FpiDeviceGoodixTls5f10 *self)
{
}

static void
fpi_device_goodixtls5f10_finalize (GObject *obj)
{
  FpiDeviceGoodixTls5f10 *self = FPI_DEVICE_GOODIXTLS5F10 (obj);
  g_clear_pointer (&self->calib, free);
  g_clear_pointer (&self->finger, free);
  if (self->enroll_infos) g_ptr_array_free (self->enroll_infos, TRUE);
  G_OBJECT_CLASS (fpi_device_goodixtls5f10_parent_class)->finalize (obj);
}

static void
fpi_device_goodixtls5f10_class_init (FpiDeviceGoodixTls5f10Class *class)
{
  FpiDeviceGoodixTlsClass *gx = FPI_DEVICE_GOODIXTLS_CLASS (class);
  FpDeviceClass *dev = FP_DEVICE_CLASS (class);
  GObjectClass *obj = G_OBJECT_CLASS (class);

  obj->finalize = fpi_device_goodixtls5f10_finalize;

  gx->interface = GOODIX_5F10_INTERFACE;
  gx->ep_in = GOODIX_5F10_EP_IN;
  gx->ep_out = GOODIX_5F10_EP_OUT;

  dev->id = "goodixtls5f10";
  dev->full_name = "Goodix TLS Fingerprint Sensor 5f10 (milanG)";
  dev->type = FP_DEVICE_TYPE_USB;
  dev->id_table = id_table;
  dev->scan_type = FP_SCAN_TYPE_PRESS;
  dev->nr_enroll_stages = ENROLL_STAGES;

  dev->probe = dev_probe;
  dev->open = dev_open;
  dev->close = dev_close;
  dev->enroll = dev_enroll;
  dev->verify = dev_verify;
  dev->identify = dev_identify;

  fpi_device_class_auto_initialize_features (dev);
}

// ---- TOD entry point ----
G_MODULE_EXPORT GType fpi_tod_shared_driver_get_type (void);
G_MODULE_EXPORT GType
fpi_tod_shared_driver_get_type (void)
{
  return fpi_device_goodixtls5f10_get_type ();
}
