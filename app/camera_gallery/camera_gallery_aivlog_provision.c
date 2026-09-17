/****************************************************************************
 * apps/examples/camera_gallery/camera_gallery_aivlog_provision.c
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/input/buttons.h>

#include <arpa/inet.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mbedtls/base64.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>
#include <netutils/cJSON.h>

#include "infra/ble_gatt.h"
#include "infra/config_store.h"
#include "infra/network_manager.h"
#include "agent_compat.h"

#include "camera_gallery.h"

#define AIVLOG_PROVISION_DEVICE_NAME       "AIVLOG-S3"
#define AIVLOG_PROVISION_MAX_MESSAGE       768
#define AIVLOG_PROVISION_SSID_SIZE         64
#define AIVLOG_PROVISION_PASSWORD_SIZE     128
#define AIVLOG_PROVISION_SCHEME_SIZE       6
#define AIVLOG_PROVISION_HOST_SIZE         256
#define AIVLOG_PROVISION_TOKEN_SIZE        256
#define AIVLOG_PROVISION_CERT_PIN_SIZE     65
#define AIVLOG_PROVISION_PATH_SIZE         96
#define AIVLOG_PROVISION_STACK_SIZE        8192
#define AIVLOG_PROVISION_CONFIG_HOST       "aivlog_cloud_host"
#define AIVLOG_PROVISION_PUBLIC_SIZE       65
#define AIVLOG_PROVISION_PUBLIC_B64_SIZE   88
#define AIVLOG_PROVISION_PUBLIC_FIELD_SIZE 128
#define AIVLOG_PROVISION_NONCE_SIZE        16
#define AIVLOG_PROVISION_NONCE_B64_SIZE    24
#define AIVLOG_PROVISION_IV_SIZE           12
#define AIVLOG_PROVISION_IV_B64_SIZE       16
#define AIVLOG_PROVISION_IV_FIELD_SIZE     64
#define AIVLOG_PROVISION_KEY_SIZE          32
#define AIVLOG_PROVISION_TAG_SIZE          16
#define AIVLOG_PROVISION_KDF_LABEL         "AIVLOG-PROVISION-v1"

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum aivlog_provision_work_e
{
  AIVLOG_PROVISION_WORK_NONE = 0,
  AIVLOG_PROVISION_WORK_HELLO,
  AIVLOG_PROVISION_WORK_CONFIGURE,
  AIVLOG_PROVISION_WORK_ENCRYPTED,
  AIVLOG_PROVISION_WORK_STATUS,
  AIVLOG_PROVISION_WORK_ERROR
};

enum aivlog_provision_error_e
{
  AIVLOG_PROVISION_ERROR_INVALID = 0,
  AIVLOG_PROVISION_ERROR_UNKNOWN_OP,
  AIVLOG_PROVISION_ERROR_TOO_LARGE,
  AIVLOG_PROVISION_ERROR_BUSY,
  AIVLOG_PROVISION_ERROR_PHYSICAL_REQUIRED,
  AIVLOG_PROVISION_ERROR_DECRYPT_FAILED
};

struct aivlog_provision_envelope_s
{
  char pub[AIVLOG_PROVISION_PUBLIC_FIELD_SIZE + 1];
  char iv[AIVLOG_PROVISION_IV_FIELD_SIZE + 1];
  char data[AIVLOG_PROVISION_MAX_MESSAGE + 1];
};

struct aivlog_provision_crypto_s
{
  mbedtls_ecdh_context ecdh;
  uint8_t nonce[AIVLOG_PROVISION_NONCE_SIZE];
};

struct aivlog_provision_config_s
{
  char ssid[AIVLOG_PROVISION_SSID_SIZE];
  char password[AIVLOG_PROVISION_PASSWORD_SIZE];
  char scheme[AIVLOG_PROVISION_SCHEME_SIZE];
  char host[AIVLOG_PROVISION_HOST_SIZE];
  char token[AIVLOG_PROVISION_TOKEN_SIZE];
  char cert_pin[AIVLOG_PROVISION_CERT_PIN_SIZE];
  char path[AIVLOG_PROVISION_PATH_SIZE];
  uint16_t port;
};

struct aivlog_provision_service_s
{
  pthread_mutex_t lock;
  pthread_cond_t cond;
  pthread_t thread;
  struct aivlog_provision_config_s pending_config;
  struct aivlog_provision_envelope_s pending_envelope;
  FAR struct aivlog_provision_crypto_s *crypto;
  char rx[AIVLOG_PROVISION_MAX_MESSAGE + 1];
  size_t rx_length;
  uint32_t connection_generation;
  uint32_t pending_generation;
  uint32_t busy_generation;
  enum aivlog_provision_work_e pending_work;
  enum aivlog_provision_error_e pending_error;
  bool initialized;
  bool thread_started;
  bool ble_initialized;
  bool stopping;
  bool rx_overflow;
  bool processing;
  bool busy_reply;
  bool configured;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct aivlog_provision_service_s g_aivlog_provision;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void aivlog_provision_clear(FAR void *buffer, size_t length)
{
  FAR volatile uint8_t *cursor = (FAR volatile uint8_t *)buffer;

  while (length-- > 0)
    {
      *cursor++ = 0;
    }
}

static int aivlog_provision_random(FAR void *context,
                                    FAR unsigned char *output,
                                    size_t length)
{
  (void)context;
  return agent_secure_random(output, length);
}

static bool aivlog_provision_json_type(FAR const cJSON *item, int type)
{
  return item != NULL && (item->type & 0xff) == type;
}

static FAR cJSON *aivlog_provision_field(FAR const cJSON *root,
                                         FAR const char *name)
{
  FAR cJSON *item;
  FAR cJSON *found = NULL;

  for (item = root->child; item != NULL; item = item->next)
    {
      if (item->string != NULL && strcmp(item->string, name) == 0)
        {
          if (found != NULL)
            {
              return NULL;
            }

          found = item;
        }
    }

  return found;
}

static bool aivlog_provision_known_field(FAR const char *name,
                                          bool configure)
{
  if (strcmp(name, "v") == 0 || strcmp(name, "op") == 0)
    {
      return true;
    }

  if (!configure)
    {
      return false;
    }

  return strcmp(name, "ssid") == 0 ||
         strcmp(name, "password") == 0 ||
         strcmp(name, "scheme") == 0 ||
         strcmp(name, "host") == 0 ||
         strcmp(name, "port") == 0 ||
         strcmp(name, "token") == 0 ||
         strcmp(name, "cert_pin") == 0 ||
         strcmp(name, "path") == 0;
}

static bool aivlog_provision_fields_valid(FAR const cJSON *root,
                                           bool configure)
{
  FAR const cJSON *item;
  int count = 0;

  for (item = root->child; item != NULL; item = item->next)
    {
      if (item->string == NULL ||
          !aivlog_provision_known_field(item->string, configure))
        {
          return false;
        }

      count++;
    }

  return count == (configure ? 10 : 2);
}

static bool aivlog_provision_copy_string(FAR char *destination,
                                          size_t destination_size,
                                          FAR const cJSON *item,
                                          size_t minimum,
                                          size_t maximum)
{
  size_t length;

  if (!aivlog_provision_json_type(item, cJSON_String) ||
      item->valuestring == NULL)
    {
      return false;
    }

  length = strlen(item->valuestring);
  if (length < minimum || length > maximum ||
      length >= destination_size)
    {
      return false;
    }

  memcpy(destination, item->valuestring, length + 1);
  return true;
}

static bool aivlog_provision_valid_host(FAR const char *host)
{
  struct in_addr ipv4;
  FAR const char *label = host;
  FAR const char *cursor;
  size_t length = strlen(host);
  size_t label_length;

  if (length == 0 || length >= 128 || strstr(host, "://") != NULL ||
      strchr(host, '/') != NULL || strchr(host, ':') != NULL)
    {
      return false;
    }

  for (cursor = host; *cursor != '\0'; cursor++)
    {
      if (isspace((unsigned char)*cursor))
        {
          return false;
        }
    }

  if (inet_pton(AF_INET, host, &ipv4) == 1)
    {
      return true;
    }

  for (cursor = host; ; cursor++)
    {
      if (*cursor != '.' && *cursor != '\0')
        {
          if (!isalnum((unsigned char)*cursor) && *cursor != '-')
            {
              return false;
            }

          continue;
        }

      label_length = (size_t)(cursor - label);
      if (label_length == 0 || label_length > 63 ||
          !isalnum((unsigned char)label[0]) ||
          !isalnum((unsigned char)label[label_length - 1]))
        {
          return false;
        }

      if (*cursor == '\0')
        {
          return true;
        }

      label = cursor + 1;
    }
}

static bool aivlog_provision_private_ipv4(FAR const char *host)
{
  struct in_addr address;
  FAR const uint8_t *bytes;

  if (inet_pton(AF_INET, host, &address) != 1)
    {
      return false;
    }

  bytes = (FAR const uint8_t *)&address.s_addr;
  return bytes[0] == 10 ||
         (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) ||
         (bytes[0] == 192 && bytes[1] == 168);
}

static bool aivlog_provision_valid_path(FAR const char *path)
{
  FAR const char *cursor;
  size_t length = strlen(path);

  if (length == 0 || length >= AIVLOG_PROVISION_PATH_SIZE ||
      path[0] != '/' || strstr(path, "..") != NULL)
    {
      return false;
    }

  for (cursor = path; *cursor != '\0'; cursor++)
    {
      if (isspace((unsigned char)*cursor))
        {
          return false;
        }
    }

  return true;
}

static bool aivlog_provision_valid_hex(FAR const char *text, size_t length)
{
  size_t index;

  if (strlen(text) != length)
    {
      return false;
    }

  for (index = 0; index < length; index++)
    {
      if (!isxdigit((unsigned char)text[index]))
        {
          return false;
        }
    }

  return true;
}

static bool aivlog_provision_valid_token(FAR const char *token)
{
  FAR const unsigned char *cursor = (FAR const unsigned char *)token;

  while (*cursor != '\0')
    {
      if (!isalnum(*cursor) && *cursor != '-' && *cursor != '_' &&
          *cursor != '.' && *cursor != '~')
        {
          return false;
        }

      cursor++;
    }

  return true;
}

static void aivlog_provision_wipe_json_string(FAR const cJSON *root,
                                               FAR const char *name)
{
  FAR cJSON *item;

  for (item = root->child; item != NULL; item = item->next)
    {
      if (item->string != NULL && strcmp(item->string, name) == 0 &&
          aivlog_provision_json_type(item, cJSON_String) &&
          item->valuestring != NULL)
        {
          aivlog_provision_clear(item->valuestring,
                                 strlen(item->valuestring));
        }
    }
}

static enum aivlog_provision_error_e
aivlog_provision_parse(FAR const char *message, size_t length,
                        FAR enum aivlog_provision_work_e *work,
                        FAR struct aivlog_provision_config_s *config)
{
  FAR const char *parse_end = NULL;
  FAR cJSON *root;
  FAR cJSON *version;
  FAR cJSON *operation;
  FAR cJSON *ssid;
  FAR cJSON *password;
  FAR cJSON *scheme;
  FAR cJSON *host;
  FAR cJSON *port;
  FAR cJSON *token;
  FAR cJSON *cert_pin;
  FAR cJSON *path;
  bool is_http;

  *work = AIVLOG_PROVISION_WORK_ERROR;
  root = cJSON_ParseWithLengthOpts(message, length + 1, &parse_end, true);
  if (!aivlog_provision_json_type(root, cJSON_Object) ||
      parse_end != message + length)
    {
      cJSON_Delete(root);
      return AIVLOG_PROVISION_ERROR_INVALID;
    }

  version = aivlog_provision_field(root, "v");
  operation = aivlog_provision_field(root, "op");
  if (!aivlog_provision_json_type(version, cJSON_Number) ||
      version->valuedouble != 1.0 || version->valueint != 1 ||
      !aivlog_provision_json_type(operation, cJSON_String) ||
      operation->valuestring == NULL)
    {
      goto out;
    }

  if (strcmp(operation->valuestring, "status") == 0)
    {
      if (aivlog_provision_fields_valid(root, false))
        {
          *work = AIVLOG_PROVISION_WORK_STATUS;
        }

      goto out;
    }

  if (strcmp(operation->valuestring, "configure") != 0)
    {
      aivlog_provision_wipe_json_string(root, "ssid");
      aivlog_provision_wipe_json_string(root, "password");
      aivlog_provision_wipe_json_string(root, "token");
      aivlog_provision_wipe_json_string(root, "cert_pin");
      cJSON_Delete(root);
      return AIVLOG_PROVISION_ERROR_UNKNOWN_OP;
    }

  if (!aivlog_provision_fields_valid(root, true))
    {
      goto out;
    }

  ssid = aivlog_provision_field(root, "ssid");
  password = aivlog_provision_field(root, "password");
  scheme = aivlog_provision_field(root, "scheme");
  host = aivlog_provision_field(root, "host");
  port = aivlog_provision_field(root, "port");
  token = aivlog_provision_field(root, "token");
  cert_pin = aivlog_provision_field(root, "cert_pin");
  path = aivlog_provision_field(root, "path");

  if (!aivlog_provision_copy_string(config->ssid, sizeof(config->ssid),
                                    ssid, 1, 63) ||
      !aivlog_provision_copy_string(config->password,
                                    sizeof(config->password),
                                    password, 8, 63) ||
      !aivlog_provision_copy_string(config->scheme,
                                    sizeof(config->scheme),
                                    scheme, 4, 5) ||
      !aivlog_provision_copy_string(config->host, sizeof(config->host),
                                    host, 1, 253) ||
      !aivlog_provision_copy_string(config->token, sizeof(config->token),
                                    token, 32, 255) ||
      !aivlog_provision_copy_string(config->cert_pin,
                                    sizeof(config->cert_pin),
                                    cert_pin, 0, 64) ||
      !aivlog_provision_copy_string(config->path, sizeof(config->path),
                                    path, 1, 95) ||
      !aivlog_provision_json_type(port, cJSON_Number) ||
      port->valuedouble < 1.0 || port->valuedouble > 65535.0 ||
      port->valuedouble != (double)port->valueint ||
      !aivlog_provision_valid_host(config->host) ||
      !aivlog_provision_valid_token(config->token) ||
      !aivlog_provision_valid_path(config->path))
    {
      goto out;
    }

  is_http = strcmp(config->scheme, "http") == 0;
  if ((!is_http && strcmp(config->scheme, "https") != 0) ||
      (is_http && (port->valueint != 8080 ||
                   config->cert_pin[0] != '\0' ||
                   !aivlog_provision_private_ipv4(config->host))) ||
      (!is_http && (port->valueint != 443 ||
                    !aivlog_provision_valid_hex(config->cert_pin, 64))))
    {
      goto out;
    }

  config->port = (uint16_t)port->valueint;
  *work = AIVLOG_PROVISION_WORK_CONFIGURE;

out:
  aivlog_provision_wipe_json_string(root, "ssid");
  aivlog_provision_wipe_json_string(root, "password");
  aivlog_provision_wipe_json_string(root, "token");
  aivlog_provision_wipe_json_string(root, "cert_pin");
  cJSON_Delete(root);
  return AIVLOG_PROVISION_ERROR_INVALID;
}

static bool aivlog_provision_envelope_fields_valid(
  FAR const cJSON *root)
{
  FAR const cJSON *item;
  int count = 0;

  for (item = root->child; item != NULL; item = item->next)
    {
      if (item->string == NULL ||
          (strcmp(item->string, "v") != 0 &&
           strcmp(item->string, "op") != 0 &&
           strcmp(item->string, "pub") != 0 &&
           strcmp(item->string, "iv") != 0 &&
           strcmp(item->string, "data") != 0))
        {
          return false;
        }

      count++;
    }

  return count == 5;
}

static enum aivlog_provision_error_e
  aivlog_provision_parse_request(
    FAR const char *message, size_t length,
    FAR enum aivlog_provision_work_e *work,
    FAR struct aivlog_provision_envelope_s *envelope)
{
  FAR const char *parse_end = NULL;
  FAR cJSON *root;
  FAR cJSON *version;
  FAR cJSON *operation;
  FAR cJSON *pub;
  FAR cJSON *iv;
  FAR cJSON *data;
  enum aivlog_provision_error_e error = AIVLOG_PROVISION_ERROR_INVALID;

  *work = AIVLOG_PROVISION_WORK_ERROR;
  root = cJSON_ParseWithLengthOpts(message, length + 1, &parse_end, true);
  if (!aivlog_provision_json_type(root, cJSON_Object) ||
      parse_end != message + length)
    {
      cJSON_Delete(root);
      return error;
    }

  version = aivlog_provision_field(root, "v");
  operation = aivlog_provision_field(root, "op");
  if (!aivlog_provision_json_type(version, cJSON_Number) ||
      version->valuedouble != 1.0 || version->valueint != 1 ||
      !aivlog_provision_json_type(operation, cJSON_String) ||
      operation->valuestring == NULL)
    {
      goto out;
    }

  if (strcmp(operation->valuestring, "status") == 0)
    {
      if (aivlog_provision_fields_valid(root, false))
        {
          *work = AIVLOG_PROVISION_WORK_STATUS;
        }

      goto out;
    }

  if (strcmp(operation->valuestring, "configure") == 0)
    {
      goto out;
    }

  if (strcmp(operation->valuestring, "encrypted") != 0)
    {
      error = AIVLOG_PROVISION_ERROR_UNKNOWN_OP;
      goto out;
    }

  if (!aivlog_provision_envelope_fields_valid(root))
    {
      goto out;
    }

  pub = aivlog_provision_field(root, "pub");
  iv = aivlog_provision_field(root, "iv");
  data = aivlog_provision_field(root, "data");
  if (!aivlog_provision_copy_string(envelope->pub,
                                    sizeof(envelope->pub), pub, 1,
                                    AIVLOG_PROVISION_PUBLIC_FIELD_SIZE) ||
      !aivlog_provision_copy_string(envelope->iv,
                                    sizeof(envelope->iv), iv, 1,
                                    AIVLOG_PROVISION_IV_FIELD_SIZE) ||
      !aivlog_provision_copy_string(envelope->data,
                                    sizeof(envelope->data), data, 1,
                                    AIVLOG_PROVISION_MAX_MESSAGE))
    {
      goto out;
    }

  *work = AIVLOG_PROVISION_WORK_ENCRYPTED;

out:
  aivlog_provision_wipe_json_string(root, "ssid");
  aivlog_provision_wipe_json_string(root, "password");
  aivlog_provision_wipe_json_string(root, "token");
  aivlog_provision_wipe_json_string(root, "cert_pin");
  cJSON_Delete(root);
  return error;
}

static void aivlog_provision_enqueue_locked(
  enum aivlog_provision_work_e work,
  enum aivlog_provision_error_e error,
  FAR const struct aivlog_provision_config_s *config,
  FAR const struct aivlog_provision_envelope_s *envelope)
{
  if ((g_aivlog_provision.processing &&
       (work == AIVLOG_PROVISION_WORK_CONFIGURE ||
        work == AIVLOG_PROVISION_WORK_ENCRYPTED)) ||
      g_aivlog_provision.pending_work != AIVLOG_PROVISION_WORK_NONE)
    {
      g_aivlog_provision.busy_reply = true;
      g_aivlog_provision.busy_generation =
        g_aivlog_provision.connection_generation;
      pthread_cond_signal(&g_aivlog_provision.cond);
      return;
    }

  g_aivlog_provision.pending_work = work;
  g_aivlog_provision.pending_error = error;
  g_aivlog_provision.pending_generation =
    g_aivlog_provision.connection_generation;
  if (work == AIVLOG_PROVISION_WORK_CONFIGURE && config != NULL)
    {
      memcpy(&g_aivlog_provision.pending_config, config, sizeof(*config));
    }
  else if (work == AIVLOG_PROVISION_WORK_ENCRYPTED && envelope != NULL)
    {
      memcpy(&g_aivlog_provision.pending_envelope, envelope,
             sizeof(*envelope));
    }

  pthread_cond_signal(&g_aivlog_provision.cond);
}

static int aivlog_provision_send(FAR const char *message,
                                   uint32_t generation);

static void aivlog_provision_crypto_free(
  FAR struct aivlog_provision_crypto_s *crypto)
{
  if (crypto == NULL)
    {
      return;
    }

  aivlog_provision_clear(crypto->nonce, sizeof(crypto->nonce));
  mbedtls_ecdh_free(&crypto->ecdh);
  aivlog_provision_clear(crypto, sizeof(*crypto));
  free(crypto);
}

static void aivlog_provision_crypto_discard(uint32_t generation)
{
  FAR struct aivlog_provision_crypto_s *crypto = NULL;

  pthread_mutex_lock(&g_aivlog_provision.lock);
  if (generation == g_aivlog_provision.connection_generation)
    {
      crypto = g_aivlog_provision.crypto;
      g_aivlog_provision.crypto = NULL;
    }

  pthread_mutex_unlock(&g_aivlog_provision.lock);
  aivlog_provision_crypto_free(crypto);
}

static FAR struct aivlog_provision_crypto_s *
  aivlog_provision_crypto_create(
    FAR uint8_t public_key[AIVLOG_PROVISION_PUBLIC_SIZE])
{
  FAR struct aivlog_provision_crypto_s *crypto;
  uint8_t encoded_point[AIVLOG_PROVISION_PUBLIC_SIZE + 1];
  size_t encoded_length = 0;
  int ret;

  crypto = calloc(1, sizeof(*crypto));
  if (crypto == NULL)
    {
      return NULL;
    }

  mbedtls_ecdh_init(&crypto->ecdh);

  ret = mbedtls_ecdh_setup(&crypto->ecdh,
                           MBEDTLS_ECP_DP_SECP256R1);
  if (ret == 0)
    {
      ret = mbedtls_ecdh_make_public(&crypto->ecdh, &encoded_length,
                                     encoded_point,
                                     sizeof(encoded_point),
                                     aivlog_provision_random, NULL);
    }

  if (ret == 0)
    {
      ret = aivlog_provision_random(NULL, crypto->nonce,
                                    sizeof(crypto->nonce));
    }

  if (ret != 0 ||
      !((encoded_length == AIVLOG_PROVISION_PUBLIC_SIZE &&
         encoded_point[0] == 0x04) ||
        (encoded_length == sizeof(encoded_point) &&
         encoded_point[0] == AIVLOG_PROVISION_PUBLIC_SIZE &&
         encoded_point[1] == 0x04)))
    {
      printf("[aivlog-provision] ECDH public failed ret=%d len=%lu\n",
             ret, (unsigned long)encoded_length);
      aivlog_provision_clear(encoded_point, sizeof(encoded_point));
      aivlog_provision_crypto_free(crypto);
      return NULL;
    }

  if (encoded_length == AIVLOG_PROVISION_PUBLIC_SIZE)
    {
      memcpy(public_key, encoded_point, AIVLOG_PROVISION_PUBLIC_SIZE);
    }
  else
    {
      memcpy(public_key, encoded_point + 1, AIVLOG_PROVISION_PUBLIC_SIZE);
    }
  aivlog_provision_clear(encoded_point, sizeof(encoded_point));
  return crypto;
}

static void aivlog_provision_hello(uint32_t generation)
{
  FAR struct aivlog_provision_crypto_s *crypto = NULL;
  uint8_t public_key[AIVLOG_PROVISION_PUBLIC_SIZE];
  char public_b64[AIVLOG_PROVISION_PUBLIC_B64_SIZE + 1];
  char nonce_b64[AIVLOG_PROVISION_NONCE_B64_SIZE + 1];
  char hello[192];
  size_t public_b64_length = 0;
  size_t nonce_b64_length = 0;
  bool installed = false;
  int ret;

  memset(public_key, 0, sizeof(public_key));
  memset(public_b64, 0, sizeof(public_b64));
  memset(nonce_b64, 0, sizeof(nonce_b64));
  memset(hello, 0, sizeof(hello));

  crypto = aivlog_provision_crypto_create(public_key);
  if (crypto == NULL)
    {
      printf("[aivlog-provision] hello key generation failed\n");
      goto out;
    }

  ret = mbedtls_base64_encode((FAR uint8_t *)public_b64,
                              sizeof(public_b64),
                              &public_b64_length, public_key,
                              sizeof(public_key));
  if (ret == 0)
    {
      ret = mbedtls_base64_encode((FAR uint8_t *)nonce_b64,
                                  sizeof(nonce_b64),
                                  &nonce_b64_length, crypto->nonce,
                                  sizeof(crypto->nonce));
    }

  if (ret != 0 ||
      public_b64_length != AIVLOG_PROVISION_PUBLIC_B64_SIZE ||
      nonce_b64_length != AIVLOG_PROVISION_NONCE_B64_SIZE)
    {
      goto out;
    }

  public_b64[public_b64_length] = '\0';
  nonce_b64[nonce_b64_length] = '\0';

  pthread_mutex_lock(&g_aivlog_provision.lock);
  if (!g_aivlog_provision.stopping &&
      generation == g_aivlog_provision.connection_generation &&
      g_aivlog_provision.crypto == NULL)
    {
      g_aivlog_provision.crypto = crypto;
      crypto = NULL;
      installed = true;
    }

  pthread_mutex_unlock(&g_aivlog_provision.lock);
  if (installed)
    {
      snprintf(hello, sizeof(hello),
               "{\"v\":1,\"status\":\"hello\",\"pub\":\"%s\","
               "\"nonce\":\"%s\"}\n",
               public_b64, nonce_b64);
      ret = aivlog_provision_send(hello, generation);
      if (ret >= 0)
        {
          printf("[aivlog-provision] hello sent generation=%lu\n",
                 (unsigned long)generation);
        }
      else
        {
          printf("[aivlog-provision] hello send failed ret=%d\n", ret);
        }
    }

out:
  aivlog_provision_crypto_free(crypto);
  aivlog_provision_clear(public_key, sizeof(public_key));
  aivlog_provision_clear(public_b64, sizeof(public_b64));
  aivlog_provision_clear(nonce_b64, sizeof(nonce_b64));
  aivlog_provision_clear(hello, sizeof(hello));
}

static void aivlog_provision_connection(bool connected, FAR void *user_data)
{
  FAR struct aivlog_provision_crypto_s *old_crypto;

  (void)user_data;
  pthread_mutex_lock(&g_aivlog_provision.lock);
  g_aivlog_provision.connection_generation++;
  if (g_aivlog_provision.connection_generation == 0)
    {
      g_aivlog_provision.connection_generation = 1;
    }

  old_crypto = g_aivlog_provision.crypto;
  g_aivlog_provision.crypto = NULL;
  aivlog_provision_clear(g_aivlog_provision.rx,
                         sizeof(g_aivlog_provision.rx));
  g_aivlog_provision.rx_length = 0;
  g_aivlog_provision.rx_overflow = false;
  g_aivlog_provision.pending_work = connected ?
    AIVLOG_PROVISION_WORK_HELLO : AIVLOG_PROVISION_WORK_NONE;
  g_aivlog_provision.pending_generation =
    g_aivlog_provision.connection_generation;
  g_aivlog_provision.busy_reply = false;
  aivlog_provision_clear(&g_aivlog_provision.pending_config,
                         sizeof(g_aivlog_provision.pending_config));
  aivlog_provision_clear(&g_aivlog_provision.pending_envelope,
                         sizeof(g_aivlog_provision.pending_envelope));
  if (connected)
    {
      pthread_cond_signal(&g_aivlog_provision.cond);
    }

  pthread_mutex_unlock(&g_aivlog_provision.lock);
  printf("[aivlog-provision] channel %s generation=%lu\n",
         connected ? "ready" : "closed",
         (unsigned long)g_aivlog_provision.connection_generation);
  aivlog_provision_crypto_free(old_crypto);
}

static void aivlog_provision_receive(FAR const uint8_t *data, uint16_t length,
                                      FAR void *user_data)
{
  struct aivlog_provision_envelope_s envelope;
  enum aivlog_provision_error_e error;
  enum aivlog_provision_work_e work;
  size_t message_length;
  uint16_t index;

  (void)user_data;
  memset(&envelope, 0, sizeof(envelope));
  pthread_mutex_lock(&g_aivlog_provision.lock);
  if (g_aivlog_provision.stopping)
    {
      pthread_mutex_unlock(&g_aivlog_provision.lock);
      return;
    }

  for (index = 0; index < length; index++)
    {
      if (data[index] != '\n')
        {
          if (g_aivlog_provision.rx_overflow)
            {
              continue;
            }

          if (g_aivlog_provision.rx_length >=
              AIVLOG_PROVISION_MAX_MESSAGE)
            {
              aivlog_provision_clear(g_aivlog_provision.rx,
                                     sizeof(g_aivlog_provision.rx));
              g_aivlog_provision.rx_length = 0;
              g_aivlog_provision.rx_overflow = true;
              continue;
            }

          g_aivlog_provision.rx[g_aivlog_provision.rx_length++] =
            (char)data[index];
          continue;
        }

      if (g_aivlog_provision.rx_overflow)
        {
          g_aivlog_provision.rx_overflow = false;
          aivlog_provision_enqueue_locked(AIVLOG_PROVISION_WORK_ERROR,
                                          AIVLOG_PROVISION_ERROR_TOO_LARGE,
                                          NULL, NULL);
          continue;
        }

      message_length = g_aivlog_provision.rx_length;
      if (message_length > 0 &&
          g_aivlog_provision.rx[message_length - 1] == '\r')
        {
          message_length--;
        }

      g_aivlog_provision.rx[message_length] = '\0';
      if (message_length == 0)
        {
          aivlog_provision_enqueue_locked(AIVLOG_PROVISION_WORK_ERROR,
                                          AIVLOG_PROVISION_ERROR_INVALID,
                                          NULL, NULL);
        }
      else
        {
          aivlog_provision_clear(&envelope, sizeof(envelope));
          error = aivlog_provision_parse_request(g_aivlog_provision.rx,
                                                  message_length, &work,
                                                  &envelope);
          if (work == AIVLOG_PROVISION_WORK_ENCRYPTED)
            {
              printf("[aivlog-provision] encrypted envelope received\n");
              aivlog_provision_enqueue_locked(work, error, NULL, &envelope);
            }
          else
            {
              aivlog_provision_enqueue_locked(work, error, NULL, NULL);
            }

          aivlog_provision_clear(&envelope, sizeof(envelope));
        }

      aivlog_provision_clear(g_aivlog_provision.rx,
                             sizeof(g_aivlog_provision.rx));
      g_aivlog_provision.rx_length = 0;
    }

  pthread_mutex_unlock(&g_aivlog_provision.lock);
  aivlog_provision_clear(&envelope, sizeof(envelope));
}

static int aivlog_provision_send(FAR const char *message,
                                   uint32_t generation)
{
  bool current;

  pthread_mutex_lock(&g_aivlog_provision.lock);
  current = !g_aivlog_provision.stopping &&
            generation == g_aivlog_provision.connection_generation;
  pthread_mutex_unlock(&g_aivlog_provision.lock);
  if (!current || !ble_gatt_is_connected())
    {
      return -ENOTCONN;
    }

  return ble_gatt_send((FAR const uint8_t *)message,
                       (uint16_t)strlen(message));
}

static void aivlog_provision_send_error(
  enum aivlog_provision_error_e error, uint32_t generation)
{
  FAR const char *code;
  char response[96];

  switch (error)
    {
      case AIVLOG_PROVISION_ERROR_UNKNOWN_OP:
        code = "unknown_op";
        break;
      case AIVLOG_PROVISION_ERROR_TOO_LARGE:
        code = "too_large";
        break;
      case AIVLOG_PROVISION_ERROR_BUSY:
        code = "busy";
        break;
      case AIVLOG_PROVISION_ERROR_PHYSICAL_REQUIRED:
        code = "physical_presence_required";
        break;
      case AIVLOG_PROVISION_ERROR_DECRYPT_FAILED:
        code = "decrypt_failed";
        break;
      default:
        code = "invalid_request";
        break;
    }

  snprintf(response, sizeof(response),
           "{\"v\":1,\"status\":\"error\",\"code\":\"%s\"}\n", code);
  aivlog_provision_send(response, generation);
}

static void aivlog_provision_send_status(uint32_t generation)
{
  char response[160];
  char ip[INET_ADDRSTRLEN];
  bool configured;
  bool connected = network_is_connected();
  FAR const char *current_ip = connected ? network_get_ip() : "";

  snprintf(ip, sizeof(ip), "%s", current_ip != NULL ? current_ip : "");
  pthread_mutex_lock(&g_aivlog_provision.lock);
  configured = g_aivlog_provision.configured;
  pthread_mutex_unlock(&g_aivlog_provision.lock);

  snprintf(response, sizeof(response),
           "{\"v\":1,\"status\":\"ready\",\"configured\":%s,"
           "\"connected\":%s,\"ip\":\"%s\"}\n",
           configured ? "true" : "false",
           connected ? "true" : "false", ip);
  aivlog_provision_send(response, generation);
}

static bool aivlog_provision_physical_presence(void)
{
  btn_buttonset_t buttons = 0;
  int fd;
  ssize_t count;

  fd = open(CONFIG_EXAMPLES_CAMERA_GALLERY_BUTTONS_DEVPATH, O_RDONLY);
  if (fd < 0)
    {
      return false;
    }

  do
    {
      count = read(fd, &buttons, sizeof(buttons));
    }
  while (count < 0 && errno == EINTR);
  close(fd);
  return count == sizeof(buttons) &&
         (buttons & CAMERA_GALLERY_BUTTON_BOOT) != 0;
}

static void aivlog_provision_configure(
  FAR struct aivlog_provision_config_s *config, uint32_t generation)
{
  bool stopping;
  bool already_configured;
  int ret;

  pthread_mutex_lock(&g_aivlog_provision.lock);
  already_configured = g_aivlog_provision.configured;
  pthread_mutex_unlock(&g_aivlog_provision.lock);
  if (already_configured && !aivlog_provision_physical_presence())
    {
      aivlog_provision_send_error(
        AIVLOG_PROVISION_ERROR_PHYSICAL_REQUIRED, generation);
      return;
    }

  aivlog_provision_send("{\"v\":1,\"status\":\"accepted\"}\n",
                        generation);
  aivlog_provision_send("{\"v\":1,\"status\":\"connecting\"}\n",
                        generation);
  ret = network_wifi_connect(NULL, config->ssid, config->password);
  if (ret < 0)
    {
      aivlog_provision_send(
        "{\"v\":1,\"status\":\"error\",\"code\":\"wifi_connect\"}\n",
        generation);
      return;
    }

  pthread_mutex_lock(&g_aivlog_provision.lock);
  stopping = g_aivlog_provision.stopping;
  pthread_mutex_unlock(&g_aivlog_provision.lock);
  if (stopping)
    {
      return;
    }

  ret = camera_gallery_aivlog_cloud_configure(
    config->scheme, config->host, config->port, config->token,
    config->cert_pin, config->path);
  if (ret < 0)
    {
      aivlog_provision_send(
        "{\"v\":1,\"status\":\"error\",\"code\":\"cloud_config\"}\n",
        generation);
      return;
    }

  pthread_mutex_lock(&g_aivlog_provision.lock);
  g_aivlog_provision.configured = true;
  pthread_mutex_unlock(&g_aivlog_provision.lock);
  camera_gallery_aivlog_cloud_wake();
  aivlog_provision_send("{\"v\":1,\"status\":\"ready\"}\n", generation);
}

static bool aivlog_provision_base64_valid(FAR const char *text,
                                           size_t length)
{
  size_t index;
  size_t padding = 0;

  if (length == 0 || (length & 3) != 0)
    {
      return false;
    }

  for (index = 0; index < length; index++)
    {
      unsigned char ch = (unsigned char)text[index];

      if ((ch >= 'A' && ch <= 'Z') ||
          (ch >= 'a' && ch <= 'z') ||
          (ch >= '0' && ch <= '9') || ch == '+' || ch == '/')
        {
          if (padding != 0)
            {
              return false;
            }
        }
      else if (ch == '=' && index >= length - 2)
        {
          padding++;
          if (padding > 2)
            {
              return false;
            }
        }
      else
        {
          return false;
        }
    }

  return true;
}

static bool aivlog_provision_decrypt(
  FAR const struct aivlog_provision_envelope_s *envelope,
  uint32_t generation,
  FAR struct aivlog_provision_config_s *config,
  FAR enum aivlog_provision_work_e *decrypted_work,
  FAR enum aivlog_provision_error_e *decrypted_error)
{
  FAR struct aivlog_provision_crypto_s *crypto = NULL;
  mbedtls_ecp_group group;
  mbedtls_ecp_point peer_point;
  mbedtls_sha256_context sha256;
  mbedtls_gcm_context gcm;
  uint8_t peer_public[AIVLOG_PROVISION_PUBLIC_SIZE];
  uint8_t peer_record[AIVLOG_PROVISION_PUBLIC_SIZE + 1];
  uint8_t iv[AIVLOG_PROVISION_IV_SIZE];
  uint8_t encrypted[AIVLOG_PROVISION_MAX_MESSAGE];
  uint8_t plaintext[AIVLOG_PROVISION_MAX_MESSAGE + 1];
  uint8_t shared[AIVLOG_PROVISION_KEY_SIZE];
  uint8_t key[AIVLOG_PROVISION_KEY_SIZE];
  size_t pub_length = 0;
  size_t iv_length = 0;
  size_t encrypted_length = 0;
  size_t shared_length = 0;
  size_t data_b64_length;
  size_t ciphertext_length;
  bool current;
  bool success = false;
  int ret = -1;

  mbedtls_ecp_group_init(&group);
  mbedtls_ecp_point_init(&peer_point);
  mbedtls_sha256_init(&sha256);
  mbedtls_gcm_init(&gcm);
  memset(peer_public, 0, sizeof(peer_public));
  memset(peer_record, 0, sizeof(peer_record));
  memset(iv, 0, sizeof(iv));
  memset(encrypted, 0, sizeof(encrypted));
  memset(plaintext, 0, sizeof(plaintext));
  memset(shared, 0, sizeof(shared));
  memset(key, 0, sizeof(key));
  aivlog_provision_clear(config, sizeof(*config));
  *decrypted_work = AIVLOG_PROVISION_WORK_ERROR;
  *decrypted_error = AIVLOG_PROVISION_ERROR_INVALID;

  pthread_mutex_lock(&g_aivlog_provision.lock);
  if (generation != g_aivlog_provision.connection_generation ||
      g_aivlog_provision.crypto == NULL)
    {
      pthread_mutex_unlock(&g_aivlog_provision.lock);
      goto out;
    }

  crypto = g_aivlog_provision.crypto;
  g_aivlog_provision.crypto = NULL;
  pthread_mutex_unlock(&g_aivlog_provision.lock);

  data_b64_length = strlen(envelope->data);
  if (strlen(envelope->pub) != AIVLOG_PROVISION_PUBLIC_B64_SIZE ||
      strlen(envelope->iv) != AIVLOG_PROVISION_IV_B64_SIZE ||
      !aivlog_provision_base64_valid(envelope->pub,
                                     AIVLOG_PROVISION_PUBLIC_B64_SIZE) ||
      !aivlog_provision_base64_valid(envelope->iv,
                                     AIVLOG_PROVISION_IV_B64_SIZE) ||
      !aivlog_provision_base64_valid(envelope->data, data_b64_length))
    {
      goto out;
    }

  ret = mbedtls_base64_decode(peer_public, sizeof(peer_public),
                              &pub_length,
                              (FAR const uint8_t *)envelope->pub,
                              AIVLOG_PROVISION_PUBLIC_B64_SIZE);
  if (ret != 0 || pub_length != AIVLOG_PROVISION_PUBLIC_SIZE ||
      peer_public[0] != 0x04)
    {
      goto out;
    }

  ret = mbedtls_base64_decode(iv, sizeof(iv), &iv_length,
                              (FAR const uint8_t *)envelope->iv,
                              AIVLOG_PROVISION_IV_B64_SIZE);
  if (ret != 0 || iv_length != AIVLOG_PROVISION_IV_SIZE)
    {
      goto out;
    }

  ret = mbedtls_base64_decode(encrypted, sizeof(encrypted),
                              &encrypted_length,
                              (FAR const uint8_t *)envelope->data,
                              data_b64_length);
  if (ret != 0 || encrypted_length <= AIVLOG_PROVISION_TAG_SIZE)
    {
      goto out;
    }

  ciphertext_length = encrypted_length - AIVLOG_PROVISION_TAG_SIZE;
  if (ciphertext_length > AIVLOG_PROVISION_MAX_MESSAGE)
    {
      goto out;
    }

  ret = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
  if (ret == 0)
    {
      ret = mbedtls_ecp_point_read_binary(&group, &peer_point,
                                          peer_public,
                                          sizeof(peer_public));
    }

  if (ret == 0)
    {
      ret = mbedtls_ecp_check_pubkey(&group, &peer_point);
    }

  if (ret != 0)
    {
      goto out;
    }

  peer_record[0] = AIVLOG_PROVISION_PUBLIC_SIZE;
  memcpy(peer_record + 1, peer_public, sizeof(peer_public));
  ret = mbedtls_ecdh_read_public(&crypto->ecdh, peer_record,
                                 sizeof(peer_record));
  if (ret == 0)
    {
      ret = mbedtls_ecdh_calc_secret(&crypto->ecdh, &shared_length,
                                     shared, sizeof(shared),
                                     aivlog_provision_random, NULL);
    }

  if (ret != 0 || shared_length != AIVLOG_PROVISION_KEY_SIZE)
    {
      goto out;
    }

  ret = mbedtls_sha256_starts(&sha256, 0);
  if (ret == 0)
    {
      ret = mbedtls_sha256_update(&sha256, shared, sizeof(shared));
    }

  if (ret == 0)
    {
      ret = mbedtls_sha256_update(&sha256, crypto->nonce,
                                  sizeof(crypto->nonce));
    }

  if (ret == 0)
    {
      ret = mbedtls_sha256_update(
        &sha256, (FAR const uint8_t *)AIVLOG_PROVISION_KDF_LABEL,
        sizeof(AIVLOG_PROVISION_KDF_LABEL) - 1);
    }

  if (ret == 0)
    {
      ret = mbedtls_sha256_finish(&sha256, key);
    }

  if (ret == 0)
    {
      ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key,
                               AIVLOG_PROVISION_KEY_SIZE * 8);
    }

  if (ret == 0)
    {
      ret = mbedtls_gcm_auth_decrypt(
        &gcm, ciphertext_length, iv, sizeof(iv), crypto->nonce,
        sizeof(crypto->nonce), encrypted + ciphertext_length,
        AIVLOG_PROVISION_TAG_SIZE, encrypted, plaintext);
    }

  if (ret == 0)
    {
      pthread_mutex_lock(&g_aivlog_provision.lock);
      current = !g_aivlog_provision.stopping &&
                generation == g_aivlog_provision.connection_generation;
      pthread_mutex_unlock(&g_aivlog_provision.lock);
      if (!current)
        {
          goto out;
        }

      plaintext[ciphertext_length] = '\0';
      *decrypted_error = aivlog_provision_parse(
        (FAR const char *)plaintext, ciphertext_length,
        decrypted_work, config);
      success = true;
    }

out:
  aivlog_provision_clear(plaintext, sizeof(plaintext));
  aivlog_provision_clear(shared, sizeof(shared));
  aivlog_provision_clear(key, sizeof(key));
  aivlog_provision_clear(peer_public, sizeof(peer_public));
  aivlog_provision_clear(peer_record, sizeof(peer_record));
  aivlog_provision_clear(iv, sizeof(iv));
  aivlog_provision_clear(encrypted, sizeof(encrypted));
  mbedtls_gcm_free(&gcm);
  mbedtls_sha256_free(&sha256);
  mbedtls_ecp_point_free(&peer_point);
  mbedtls_ecp_group_free(&group);
  aivlog_provision_crypto_free(crypto);
  if (!success)
    {
      aivlog_provision_clear(config, sizeof(*config));
      *decrypted_work = AIVLOG_PROVISION_WORK_ERROR;
      *decrypted_error = AIVLOG_PROVISION_ERROR_DECRYPT_FAILED;
    }

  return success;
}

static FAR void *aivlog_provision_reconnect(FAR void *argument)
{
  (void)argument;
  network_wifi_reconnect();
  return NULL;
}

static FAR void *aivlog_provision_worker(FAR void *argument)
{
  struct aivlog_provision_config_s config;
  struct aivlog_provision_envelope_s envelope;
  enum aivlog_provision_work_e work;
  enum aivlog_provision_work_e decrypted_work;
  enum aivlog_provision_error_e error;
  enum aivlog_provision_error_e decrypted_error;
  uint32_t generation;
  uint32_t busy_generation;
  bool busy_reply;

  (void)argument;
  memset(&config, 0, sizeof(config));
  memset(&envelope, 0, sizeof(envelope));

  for (; ; )
    {
      pthread_mutex_lock(&g_aivlog_provision.lock);
      while (!g_aivlog_provision.stopping &&
             g_aivlog_provision.pending_work ==
               AIVLOG_PROVISION_WORK_NONE &&
             !g_aivlog_provision.busy_reply)
        {
          pthread_cond_wait(&g_aivlog_provision.cond,
                            &g_aivlog_provision.lock);
        }

      if (g_aivlog_provision.stopping)
        {
          pthread_mutex_unlock(&g_aivlog_provision.lock);
          break;
        }

      work = g_aivlog_provision.pending_work;
      error = g_aivlog_provision.pending_error;
      generation = g_aivlog_provision.pending_generation;
      busy_generation = g_aivlog_provision.busy_generation;
      busy_reply = g_aivlog_provision.busy_reply;
      g_aivlog_provision.pending_work = AIVLOG_PROVISION_WORK_NONE;
      g_aivlog_provision.busy_reply = false;
      g_aivlog_provision.processing = work != AIVLOG_PROVISION_WORK_NONE;
      if (work == AIVLOG_PROVISION_WORK_CONFIGURE)
        {
          memcpy(&config, &g_aivlog_provision.pending_config,
                 sizeof(config));
          aivlog_provision_clear(&g_aivlog_provision.pending_config,
                                 sizeof(g_aivlog_provision.pending_config));
        }
      else if (work == AIVLOG_PROVISION_WORK_ENCRYPTED)
        {
          memcpy(&envelope, &g_aivlog_provision.pending_envelope,
                 sizeof(envelope));
          aivlog_provision_clear(&g_aivlog_provision.pending_envelope,
                                 sizeof(g_aivlog_provision.pending_envelope));
        }

      pthread_mutex_unlock(&g_aivlog_provision.lock);

      switch (work)
        {
          case AIVLOG_PROVISION_WORK_HELLO:
            aivlog_provision_hello(generation);
            break;
          case AIVLOG_PROVISION_WORK_CONFIGURE:
            aivlog_provision_configure(&config, generation);
            aivlog_provision_clear(&config, sizeof(config));
            break;
          case AIVLOG_PROVISION_WORK_ENCRYPTED:
            if (!aivlog_provision_decrypt(&envelope, generation,
                                          &config, &decrypted_work,
                                          &decrypted_error))
              {
                printf("[aivlog-provision] decrypt failed\n");
                aivlog_provision_send_error(
                  AIVLOG_PROVISION_ERROR_DECRYPT_FAILED, generation);
              }
            else if (decrypted_work == AIVLOG_PROVISION_WORK_CONFIGURE)
              {
                printf("[aivlog-provision] decrypt verified\n");
                aivlog_provision_configure(&config, generation);
              }
            else
              {
                aivlog_provision_send_error(decrypted_error, generation);
              }

            aivlog_provision_clear(&config, sizeof(config));
            aivlog_provision_clear(&envelope, sizeof(envelope));
            break;
          case AIVLOG_PROVISION_WORK_STATUS:
            aivlog_provision_send_status(generation);
            break;
          case AIVLOG_PROVISION_WORK_ERROR:
            aivlog_provision_crypto_discard(generation);
            aivlog_provision_send_error(error, generation);
            break;
          default:
            break;
        }

      if (busy_reply)
        {
          aivlog_provision_send_error(AIVLOG_PROVISION_ERROR_BUSY,
                                      busy_generation);
        }

      pthread_mutex_lock(&g_aivlog_provision.lock);
      g_aivlog_provision.processing = false;
      pthread_mutex_unlock(&g_aivlog_provision.lock);
    }

  aivlog_provision_clear(&config, sizeof(config));
  aivlog_provision_clear(&envelope, sizeof(envelope));
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int camera_gallery_aivlog_provision_initialize(void)
{
  struct aivlog_provision_service_s *service = &g_aivlog_provision;
  ble_gatt_config_t ble_config;
  pthread_attr_t attributes;
  char endpoint[AIVLOG_PROVISION_HOST_SIZE];
  bool attributes_initialized = false;
  int ret;

  if (service->initialized)
    {
      return OK;
    }

  if (config_store_init() < 0)
    {
      return ERROR;
    }

  memset(endpoint, 0, sizeof(endpoint));
  memset(service, 0, sizeof(*service));
  ret = pthread_mutex_init(&service->lock, NULL);
  if (ret != 0)
    {
      errno = ret;
      return ERROR;
    }

  ret = pthread_cond_init(&service->cond, NULL);
  if (ret != 0)
    {
      pthread_mutex_destroy(&service->lock);
      errno = ret;
      return ERROR;
    }

  service->initialized = true;
  if (claw_config_get(AIVLOG_PROVISION_CONFIG_HOST, endpoint,
                      sizeof(endpoint)) == OK && endpoint[0] != '\0')
    {
      service->configured = true;
    }

  aivlog_provision_clear(endpoint, sizeof(endpoint));
  ret = pthread_attr_init(&attributes);
  if (ret == 0)
    {
      attributes_initialized = true;
      ret = pthread_attr_setstacksize(&attributes,
                                      AIVLOG_PROVISION_STACK_SIZE);
    }

  if (ret == 0)
    {
      ret = pthread_create(&service->thread, &attributes,
                           aivlog_provision_worker, NULL);
    }

  if (attributes_initialized)
    {
      pthread_attr_destroy(&attributes);
    }

  if (ret != 0)
    {
      pthread_cond_destroy(&service->cond);
      pthread_mutex_destroy(&service->lock);
      memset(service, 0, sizeof(*service));
      errno = ret;
      return ERROR;
    }

  service->thread_started = true;
  memset(&ble_config, 0, sizeof(ble_config));
  ble_config.device_name = AIVLOG_PROVISION_DEVICE_NAME;
  ble_config.recv_cb = aivlog_provision_receive;
  ble_config.conn_cb = aivlog_provision_connection;
  ble_config.user_data = service;
  ret = ble_gatt_init(&ble_config);
  if (ret == 0)
    {
      service->ble_initialized = true;
    }
  else
    {
      printf("[aivlog-provision] BLE unavailable: %d\n", ret);
    }

  if (service->configured &&
      agent_task_create(aivlog_provision_reconnect, "aivlog_wifi",
                        4096, NULL, 100) < 0)
    {
      printf("[aivlog-provision] automatic WiFi reconnect unavailable\n");
    }

  return OK;
}

void camera_gallery_aivlog_provision_finalize(void)
{
  struct aivlog_provision_service_s *service = &g_aivlog_provision;
  FAR struct aivlog_provision_crypto_s *crypto;

  if (!service->initialized)
    {
      return;
    }

  pthread_mutex_lock(&service->lock);
  service->stopping = true;
  crypto = service->crypto;
  service->crypto = NULL;
  aivlog_provision_clear(service->rx, sizeof(service->rx));
  service->rx_length = 0;
  service->rx_overflow = false;
  service->pending_work = AIVLOG_PROVISION_WORK_NONE;
  aivlog_provision_clear(&service->pending_config,
                         sizeof(service->pending_config));
  aivlog_provision_clear(&service->pending_envelope,
                         sizeof(service->pending_envelope));
  pthread_cond_signal(&service->cond);
  pthread_mutex_unlock(&service->lock);
  aivlog_provision_crypto_free(crypto);

  if (service->ble_initialized)
    {
      ble_gatt_deinit();
      service->ble_initialized = false;
    }

  if (service->thread_started)
    {
      pthread_join(service->thread, NULL);
    }

  aivlog_provision_clear(service->rx, sizeof(service->rx));
  aivlog_provision_clear(&service->pending_config,
                         sizeof(service->pending_config));
  aivlog_provision_clear(&service->pending_envelope,
                         sizeof(service->pending_envelope));
  pthread_cond_destroy(&service->cond);
  pthread_mutex_destroy(&service->lock);
  memset(service, 0, sizeof(*service));
}
