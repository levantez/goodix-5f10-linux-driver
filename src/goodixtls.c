// Goodix Tls driver for libfprint

// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
// Copyright (C) 2021 Natasha England-Elbro <natasha@natashaee.me>

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include <arpa/inet.h>
#include <errno.h>
#include <glib.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "drivers_api.h"
#include "fp-device.h"
#include "fpi-device.h"
#include "glibconfig.h"
#include "goodix.h"
#include "goodixtls.h"

static const guint8 goodix_5f10_default_psk[GOODIX_5F10_PSK_LENGTH] = {
  0xc2, 0x7e, 0x89, 0x4f, 0x34, 0x51, 0xf8, 0x26,
  0x51, 0x8f, 0x18, 0x4e, 0x2a, 0xdc, 0x51, 0x2c,
  0x4b, 0x68, 0x02, 0x60, 0x5a, 0x65, 0x28, 0xe8,
  0xd2, 0xfc, 0xe9, 0x74, 0xac, 0x1a, 0x4b, 0x85,
};

static GError *
err_from_ssl (void)
{
  GError *err = malloc (sizeof (GError));
  unsigned long code = ERR_get_error ();

  err->code = code;
  const char *msg = ERR_reason_error_string (code);

  err->message = malloc (strlen (msg));
  strcpy (err->message, msg);
  return err;
}

static unsigned int
tls_server_psk_server_callback (SSL           *ssl,
                                const char    *identity,
                                unsigned char *psk,
                                unsigned int   max_psk_len)
{
  SSL_CTX *ctx = SSL_get_SSL_CTX (ssl);
  GoodixTlsServer *self = SSL_CTX_get_app_data (ctx);
  const int len = GOODIX_5F10_PSK_LENGTH;

  (void) identity;

  fp_dbg ("PSK WANTED %d", max_psk_len);
  if (len > max_psk_len)
    {
      fp_err ("max psk length (%d) too short (needs %d)", max_psk_len, len);
      return 0;
    }

  if (!self)
    return 0;

  memcpy (psk, self->psk, len);

  return len;
}

static SSL_CTX *
tls_server_create_ctx (void)
{
  const SSL_METHOD *method;

  method = TLS_server_method ();

  SSL_CTX *ctx = SSL_CTX_new (method);

  if (!ctx)
    return NULL;

  return ctx;
}

static void
tls_server_config_ctx (SSL_CTX *ctx)
{
  SSL_CTX_set_ecdh_auto (ctx, 1);
  SSL_CTX_set_dh_auto (ctx, 1);
  SSL_CTX_set_cipher_list (ctx, "ALL");
  SSL_CTX_set_min_proto_version (ctx, TLS1_2_VERSION);
  SSL_CTX_set_max_proto_version (ctx, TLS1_2_VERSION);
  SSL_CTX_set_psk_server_callback (ctx, tls_server_psk_server_callback);
}

int
goodix_tls_client_write (GoodixTlsServer *self, guint8 *data, guint16 length)
{
  return write (self->client_fd, data, length * sizeof (guint8));
}
int
goodix_tls_client_read (GoodixTlsServer *self, guint8 *data, guint16 length)
{
  return read (self->client_fd, data, length * sizeof (guint8));
}

int
goodix_tls_server_read (GoodixTlsServer *self, guint8 *data,
                        guint32 length, GError **error)
{
  int retr = SSL_read (self->ssl_layer, data, length * sizeof (guint8));

  if (retr <= 0)
    *error = err_from_ssl ();
  return retr;
}

static void
tls_config_ssl (SSL *ssl)
{
  SSL_set_min_proto_version (ssl, TLS1_2_VERSION);
  SSL_set_max_proto_version (ssl, TLS1_2_VERSION);
  SSL_set_psk_server_callback (ssl, tls_server_psk_server_callback);
  SSL_set_cipher_list (ssl, "ALL");
}

static void *
goodix_tls_init_serve (void *me)
{
  GoodixTlsServer *self = me;

  fp_dbg ("TLS server waiting to accept...");
  int retr = SSL_accept (self->ssl_layer);

  fp_dbg ("TLS server accept done");
  if (retr <= 0)
    fp_err ("server ready failed: %s", ERR_reason_error_string (ERR_get_error ()));
  else
    fp_dbg ("TLS connection ready");
  return NULL;
}

gboolean
goodix_tls_server_deinit (GoodixTlsServer *self, GError **error)
{
  // Wake a thread blocked in SSL_accept before joining it.
  shutdown (self->sock_fd, SHUT_RDWR);
  pthread_join (self->serve_thread, NULL);

  SSL_shutdown (self->ssl_layer);
  SSL_free (self->ssl_layer);

  close (self->client_fd);
  close (self->sock_fd);

  SSL_CTX_free (self->ssl_ctx);
  OPENSSL_cleanse (self->psk, sizeof (self->psk));

  return TRUE;
}

gboolean
goodix_tls_server_init (GoodixTlsServer *self, GError **error)
{
  const char *psk_path = g_getenv ("GOODIX_5F10_PSK_FILE");
  if (!psk_path || !*psk_path)
    psk_path = "/var/lib/fprint/goodix-5f10/psk";

  g_autofree gchar *psk_data = NULL;
  g_autoptr(GError) psk_error = NULL;
  gsize psk_length = 0;
  const guint8 *psk = goodix_5f10_default_psk;
  const char *psk_source = "built-in default";
  if (!g_file_get_contents (psk_path, &psk_data, &psk_length, &psk_error))
    {
      if (!g_error_matches (psk_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          g_prefix_error (&psk_error, "Unable to load Goodix 5F10 PSK from %s: ",
                          psk_path);
          g_propagate_error (error, g_steal_pointer (&psk_error));
          return FALSE;
        }
      g_clear_error (&psk_error);
    }
  else
    {
      if (psk_length != GOODIX_5F10_PSK_LENGTH)
        {
          g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                       "Goodix 5F10 PSK file %s must contain exactly %u raw bytes",
                       psk_path, GOODIX_5F10_PSK_LENGTH);
          return FALSE;
        }
      psk = (const guint8 *) psk_data;
      psk_source = psk_path;
    }
  memcpy (self->psk, psk, sizeof (self->psk));
  if (psk_data)
    OPENSSL_cleanse (psk_data, psk_length);
  fp_dbg ("Using Goodix 5F10 PSK from %s", psk_source);

  SSL_load_error_strings ();
  OpenSSL_add_ssl_algorithms ();
  SSL_library_init ();
  self->ssl_ctx = tls_server_create_ctx ();
  if (self->ssl_ctx == NULL)
    {
      OPENSSL_cleanse (self->psk, sizeof (self->psk));
      g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                           "Unable to create TLS server context");
      return FALSE;
    }
  SSL_CTX_set_app_data (self->ssl_ctx, self);
  tls_server_config_ctx (self->ssl_ctx);

  int socks[2] = {0, 0};
  if (socketpair (AF_UNIX, SOCK_STREAM, 0, socks) != 0)
    {
      g_set_error (error, G_FILE_ERROR, errno,
                   "failed to create socket pair: %s", strerror (errno));
      SSL_CTX_free (self->ssl_ctx);
      self->ssl_ctx = NULL;
      OPENSSL_cleanse (self->psk, sizeof (self->psk));
      return FALSE;
    }
  self->sock_fd = socks[0];
  self->client_fd = socks[1];

  self->ssl_layer = SSL_new (self->ssl_ctx);
  if (self->ssl_layer == NULL)
    {
      close (self->client_fd);
      close (self->sock_fd);
      self->client_fd = -1;
      self->sock_fd = -1;
      SSL_CTX_free (self->ssl_ctx);
      self->ssl_ctx = NULL;
      OPENSSL_cleanse (self->psk, sizeof (self->psk));
      g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                           "Unable to create TLS session");
      return FALSE;
    }
  tls_config_ssl (self->ssl_layer);
  SSL_set_fd (self->ssl_layer, self->sock_fd);

  int thread_error = pthread_create (&self->serve_thread, NULL,
                                     goodix_tls_init_serve, self);
  if (thread_error != 0)
    {
      close (self->client_fd);
      close (self->sock_fd);
      self->client_fd = -1;
      self->sock_fd = -1;
      SSL_free (self->ssl_layer);
      self->ssl_layer = NULL;
      SSL_CTX_free (self->ssl_ctx);
      self->ssl_ctx = NULL;
      OPENSSL_cleanse (self->psk, sizeof (self->psk));
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (thread_error),
                   "failed to create TLS server thread: %s",
                   g_strerror (thread_error));
      return FALSE;
    }

  return TRUE;
}
