// Тест-харнес для TOD-драйвера 5f10 БЕЗ установки в систему.
// Запуск: FP_TOD_DRIVERS_DIR=$PWD/build ./tod_test [list|enroll <name>|verify]
// list   — перечислить устройства (грузит .so, матчит USB ID; палец не нужен)
// enroll — открыть, зарегистрировать палец (20 прикладываний), сохранить в /tmp/<name>.fp
// verify — открыть, сверить палец с /tmp/<name>.fp
#include <fprint.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <unistd.h>

static void
on_enroll_progress (FpDevice *dev, gint completed, FpPrint *print,
                    gpointer data, GError *err)
{
  gint total = GPOINTER_TO_INT (data);
  printf ("  прогресс: %d/%d\n", completed, total);
}

static FpDevice *
first_device (FpContext *ctx)
{
  fp_context_enumerate (ctx);
  GPtrArray *devs = fp_context_get_devices (ctx);
  if (!devs || devs->len == 0)
    return NULL;
  return g_ptr_array_index (devs, 0);
}

int
main (int argc, char **argv)
{
  const char *cmd = argc > 1 ? argv[1] : "list";
  g_autoptr (FpContext) ctx = fp_context_new ();
  g_autoptr (GError) err = NULL;

  fp_context_enumerate (ctx);
  GPtrArray *devs = fp_context_get_devices (ctx);
  printf ("устройств найдено: %u\n", devs ? devs->len : 0);
  for (guint i = 0; devs && i < devs->len; i++)
    {
      FpDevice *d = g_ptr_array_index (devs, i);
      printf ("  [%u] driver=%s name=\"%s\"\n", i,
              fp_device_get_driver (d), fp_device_get_name (d));
    }
  if (!devs || devs->len == 0)
    {
      printf ("НЕТ устройств — драйвер не подхватился или USB ID не совпал\n");
      return 1;
    }
  if (strcmp (cmd, "list") == 0)
    return 0;

  FpDevice *dev = g_ptr_array_index (devs, 0);
  printf ("открываю устройство…\n");
  if (!fp_device_open_sync (dev, NULL, &err))
    {
      printf ("open FAILED: %s\n", err->message);
      return 2;
    }
  printf ("устройство открыто OK\n");

  int rc = 0;
  if (strcmp (cmd, "enroll") == 0)
    {
      const char *name = argc > 2 ? argv[2] : "test";
      gint stages = fp_device_get_nr_enroll_stages (dev);
      FpPrint *tmpl = fp_print_new (dev);
      fp_print_set_finger (tmpl, FP_FINGER_RIGHT_INDEX);
      fp_print_set_username (tmpl, name);
      printf ("ПРИКЛАДЫВАЙ палец %d раз…\n", stages);
      FpPrint *out = fp_device_enroll_sync (dev, tmpl, NULL,
                                            on_enroll_progress,
                                            GINT_TO_POINTER (stages), &err);
      if (!out)
        {
          printf ("enroll FAILED: %s\n", err ? err->message : "?");
          rc = 3;
        }
      else
        {
          g_autofree char *path = g_strdup_printf ("/tmp/%s.fp", name);
          gsize len;
          g_autofree guint8 *data = NULL;
          fp_print_serialize (out, &data, &len, &err);
          g_file_set_contents (path, (char *) data, len, &err);
          printf ("enroll OK -> %s (%zu байт)\n", path, len);
        }
    }
  else if (strcmp (cmd, "dataset") == 0)
    {
      // одна команда: снять N кадров подряд в /tmp/ds/<prefix>N.pgm (для бенчмарка SIGFM)
      const char *prefix = argc > 2 ? argv[2] : "f";
      int count = argc > 3 ? atoi (argv[3]) : 5;
      g_mkdir_with_parents ("/tmp/ds", 0755);
      g_autofree char *td = NULL; gsize tl;
      FpPrint *tmpl = NULL;
      if (g_file_get_contents ("/tmp/levantez.fp", &td, &tl, NULL))
        tmpl = fp_print_deserialize ((guint8 *) td, tl, NULL);
      if (!tmpl) { tmpl = fp_print_new (dev); fp_print_set_finger (tmpl, FP_FINGER_RIGHT_INDEX);
                   fp_print_set_username (tmpl, "x"); }
      for (int i = 1; i <= count; i++)
        {
          printf ("[%d/%d] приложи палец полностью, держи, потом убери…\n", i, count);
          fflush (stdout);
          gboolean match = FALSE;
          g_clear_error (&err);
          fp_device_verify_sync (dev, tmpl, NULL, NULL, NULL, &match, NULL, &err);
          g_clear_error (&err);
          g_autofree char *out = g_strdup_printf ("/tmp/ds/%s%d.pgm", prefix, i);
          g_autofree char *d = NULL; gsize l;
          if (g_file_get_contents ("/tmp/gx5f10_last.pgm", &d, &l, NULL)
              && g_file_set_contents (out, d, l, NULL))
            printf ("  -> %s\n", out);
          else
            printf ("  пропуск (кадр не получен)\n");
        }
      printf ("готово: набор /tmp/ds/%s*.pgm\n", prefix);
    }
  else if (strcmp (cmd, "grab") == 0)
    {
      // один захват в PGM для набора данных (драйвер сам пишет /tmp/gx5f10_last.pgm)
      const char *out = argc > 2 ? argv[2] : "/tmp/grab.pgm";
      g_autofree char *td = NULL; gsize tl;
      FpPrint *tmpl = NULL;
      if (g_file_get_contents ("/tmp/levantez.fp", &td, &tl, NULL))
        tmpl = fp_print_deserialize ((guint8 *) td, tl, NULL);
      if (!tmpl) { tmpl = fp_print_new (dev); fp_print_set_finger (tmpl, FP_FINGER_RIGHT_INDEX);
                   fp_print_set_username (tmpl, "x"); }
      gboolean match = FALSE;
      printf ("ПРИЛОЖИ палец для захвата…\n");
      fp_device_verify_sync (dev, tmpl, NULL, NULL, NULL, &match, NULL, &err);
      g_clear_error (&err);
      g_autofree char *d = NULL; gsize l;
      if (g_file_get_contents ("/tmp/gx5f10_last.pgm", &d, &l, NULL)
          && g_file_set_contents (out, d, l, NULL))
        printf ("захвачено -> %s\n", out);
      else
        printf ("нет кадра (касание не распозналось?)\n");
    }
  else if (strcmp (cmd, "verify") == 0)
    {
      const char *name = argc > 2 ? argv[2] : "test";
      g_autofree char *path = g_strdup_printf ("/tmp/%s.fp", name);
      g_autofree char *data = NULL;
      gsize len;
      if (!g_file_get_contents (path, &data, &len, &err))
        {
          printf ("нет шаблона %s\n", path);
          rc = 4;
        }
      else
        {
          FpPrint *tmpl = fp_print_deserialize ((guint8 *) data, len, &err);
          gboolean match = FALSE;
          gboolean done = FALSE;
          // как pam_fprintd: повторяем при retry-ошибках (плохой кадр минуций)
          for (int attempt = 1; attempt <= 6 && !done; attempt++)
            {
              g_clear_error (&err);
              printf ("[%d/6] ПРИКЛАДЫВАЙ палец для сверки…\n", attempt);
              if (fp_device_verify_sync (dev, tmpl, NULL, NULL, NULL,
                                         &match, NULL, &err))
                {
                  printf (match ? "*** СОВПАЛО ***\n" : "не совпало\n");
                  rc = match ? 0 : 1;
                  done = TRUE;
                }
              else if (err && err->domain == fp_device_retry_quark ())
                {
                  printf ("  плохой кадр (%s) — приложи ещё раз\n", err->message);
                }
              else
                {
                  printf ("verify FAILED: %s\n", err ? err->message : "?");
                  rc = 5;
                  done = TRUE;
                }
            }
          if (!done)
            printf ("не удалось получить годный кадр за 6 попыток\n");
        }
    }

  fp_device_close_sync (dev, NULL, NULL);
  fflush (stdout);
  _exit (rc);  // минуем atexit OPENSSL_cleanup (гонка с фон-потоками при выходе харнеса)
}
