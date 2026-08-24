// Диагностика извлечения минуций: прогоняет РОВНО libfprint-детект (fp_image_detect_minutiae,
// внутри NBIS mindtct) на PGM-кадре со свипом ppmm. Печатает число минуций для каждого ppmm
// и выгружает координаты для лучшего ppmm в <pgm>.min.txt (для наложения на картинку).
//   mindtct_probe <frame.pgm>
#include <fprint.h>
#include "fpi-image.h"
#include "fpi-minutiae.h"
#include <glib.h>
#include <stdio.h>
#include <string.h>

static GMainLoop *loop;
static int g_count;

static void
on_detect (GObject *src, GAsyncResult *res, gpointer u)
{
  GError *e = NULL;
  fp_image_detect_minutiae_finish (FP_IMAGE (src), res, &e);
  GPtrArray *m = fp_image_get_minutiae (FP_IMAGE (src));
  g_count = (m ? (int) m->len : 0);
  if (e) g_clear_error (&e);
  g_main_loop_quit (loop);
}

static FpImage *
load_pgm (const char *path, int *w, int *h)
{
  char *raw; gsize len; GError *e = NULL;
  if (!g_file_get_contents (path, &raw, &len, &e)) { printf ("нет %s\n", path); return NULL; }
  int W, H, MX, off = 0;
  if (sscanf (raw, "P5 %d %d %d%n", &W, &H, &MX, &off) < 3) { printf ("не PGM\n"); return NULL; }
  off++; // пробел/newline после maxval
  FpImage *img = fp_image_new (W, H);
  memcpy (img->data, raw + off, (gsize) W * H);
  *w = W; *h = H;
  g_free (raw);
  return img;
}

static int
detect_at (const char *path, double ppmm, GPtrArray **out_min)
{
  int w, h;
  FpImage *img = load_pgm (path, &w, &h);
  if (!img) return -1;
  img->ppmm = ppmm;
  g_count = -1;
  fp_image_detect_minutiae (img, NULL, on_detect, NULL);
  g_main_loop_run (loop);
  if (out_min)
    {
      GPtrArray *m = fp_image_get_minutiae (img);
      *out_min = m ? g_ptr_array_ref (m) : NULL;
    }
  g_object_unref (img);
  return g_count;
}

int
main (int argc, char **argv)
{
  const char *path = argc > 1 ? argv[1] : "/tmp/gx5f10_last.pgm";
  loop = g_main_loop_new (NULL, FALSE);

  int w, h; FpImage *probe = load_pgm (path, &w, &h);
  if (!probe) return 1;
  printf ("кадр %s: %dx%d, дефолтный ppmm=%.3f\n", path, w, h, fp_image_get_ppmm (probe));
  g_object_unref (probe);

  double ppmms[] = { 5, 8, 10, 12, 15, 19.685, 25, 30, 40, 50 };
  double best_ppmm = 0; int best = -1;
  printf ("\nСВИП ppmm -> число минуций:\n");
  for (guint i = 0; i < G_N_ELEMENTS (ppmms); i++)
    {
      int c = detect_at (path, ppmms[i], NULL);
      printf ("  ppmm=%-7.3f -> минуций: %d\n", ppmms[i], c);
      if (c > best) { best = c; best_ppmm = ppmms[i]; }
    }
  printf ("\nЛУЧШИЙ: ppmm=%.3f -> %d минуций\n", best_ppmm, best);

  // выгрузить координаты минуций для лучшего ppmm
  GPtrArray *min = NULL;
  detect_at (path, best_ppmm, &min);
  if (min && min->len)
    {
      char out[512]; snprintf (out, sizeof out, "%s.min.txt", path);
      FILE *f = fopen (out, "w");
      for (guint i = 0; i < min->len; i++)
        {
          struct fp_minutia *m = g_ptr_array_index (min, i);
          fprintf (f, "%d %d %d %.3f %d\n", m->x, m->y, m->direction,
                   m->reliability, m->type);
        }
      fclose (f);
      printf ("координаты минуций -> %s (%u шт)\n", out, min->len);
    }
  return 0;
}
