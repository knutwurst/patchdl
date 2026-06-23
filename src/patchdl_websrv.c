#include "patchdl_websrv.h"

#include "patchdl_assets.h"

#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct MHD_Daemon* web_daemon;

static const char status_json[] =
  "{"
  "\"firmware\":\"11.60\","
  "\"firmware_build\":\"0x11600000\","
  "\"dns_guard\":\"Active\","
  "\"resolver\":\"Internal allowlist\","
  "\"free_space_mb\":64218,"
  "\"download_dir\":\"/mnt/usb0/patches\""
  "}";

static const char config_json[] =
  "{"
  "\"default_policy\":\"deny\","
  "\"download_dir\":\"/mnt/usb0/patches\","
  "\"install_after_download\":false,"
  "\"delete_pkg_after_install\":false,"
  "\"source_policy\":{"
  "\"official\":{\"allow_check\":true,\"allow_download\":true,\"allow_install\":true},"
  "\"external\":{\"allow_check\":true,\"allow_download\":true,\"allow_install\":true},"
  "\"shadowmount\":{\"allow_check\":true,\"allow_download\":true,\"allow_install\":false},"
  "\"unknown\":{\"allow_check\":true,\"allow_download\":false,\"allow_install\":false}"
  "},"
  "\"cdn_allowlist\":["
  "\"sgst.prod.dl.playstation.net\","
  "\"gst.prod.dl.playstation.net\","
  "\"gs2.ww.prod.dl.playstation.net\""
  "]"
  "}";

static const char titles_json[] =
  "["
  "{"
  "\"title_id\":\"PPSA01628_00\","
  "\"name\":\"Call of Duty Black Ops Cold War\","
  "\"content_id\":\"UP0002-PPSA01628_00-CODCWTHEGAME0001\","
  "\"installed_version\":\"01.032.000\","
  "\"compatible_version\":\"01.041.000\","
  "\"latest_version\":\"01.041.000\","
  "\"latest_required_fw\":\"11.60\","
  "\"source_type\":\"official\","
  "\"source_path\":\"/system_ex/app/PPSA01628_00\","
  "\"mount_from\":\"/dev/ssd0.system_ex\","
  "\"enabled\":true,"
  "\"mode\":\"latest_compatible\","
  "\"queued\":false,"
  "\"status\":\"available\""
  "},"
  "{"
  "\"title_id\":\"PPSA90001_00\","
  "\"name\":\"Shadowmounted Test Title\","
  "\"content_id\":\"UP0000-PPSA90001_00-SHADOWMOUNT0001\","
  "\"installed_version\":\"01.000.000\","
  "\"compatible_version\":\"01.006.000\","
  "\"latest_version\":\"01.009.000\","
  "\"latest_required_fw\":\"12.00\","
  "\"source_type\":\"shadowmount\","
  "\"source_path\":\"/system_ex/app/PPSA90001_00\","
  "\"mount_from\":\"/mnt/usb0/itemzflow/Shadowmounted Test Title\","
  "\"enabled\":true,"
  "\"mode\":\"download_only\","
  "\"queued\":false,"
  "\"status\":\"available\""
  "},"
  "{"
  "\"title_id\":\"PPSA08329_00\","
  "\"name\":\"Example Future Game\","
  "\"content_id\":\"EP0000-PPSA08329_00-FUTUREPATCH00001\","
  "\"installed_version\":\"01.000.000\","
  "\"compatible_version\":null,"
  "\"latest_version\":\"01.012.000\","
  "\"latest_required_fw\":\"12.50\","
  "\"source_type\":\"unknown\","
  "\"source_path\":\"/system_ex/app/PPSA08329_00\","
  "\"mount_from\":\"\","
  "\"enabled\":false,"
  "\"mode\":\"disabled\","
  "\"queued\":false,"
  "\"status\":\"blocked\""
  "}"
  "]";

static const char downloads_json[] =
  "[]";

static enum MHD_Result
queue_buffer(struct MHD_Connection* conn, unsigned int status,
             const char* mime, const void* data, size_t size) {
  struct MHD_Response* response;
  enum MHD_Result result;

  response = MHD_create_response_from_buffer(size, (void*)data,
                                             MHD_RESPMEM_PERSISTENT);
  if(!response) {
    return MHD_NO;
  }

  MHD_add_response_header(response, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN,
                          "*");
  MHD_add_response_header(response, MHD_HTTP_HEADER_CACHE_CONTROL,
                          "no-store");
  if(mime) {
    MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, mime);
  }

  result = MHD_queue_response(conn, status, response);
  MHD_destroy_response(response);
  return result;
}

static enum MHD_Result
queue_text(struct MHD_Connection* conn, unsigned int status, const char* text) {
  return queue_buffer(conn, status, "text/plain; charset=utf-8", text,
                      strlen(text));
}

static enum MHD_Result
queue_json(struct MHD_Connection* conn, unsigned int status, const char* json) {
  return queue_buffer(conn, status, "application/json", json, strlen(json));
}

static enum MHD_Result
queue_asset(struct MHD_Connection* conn, const char* url) {
  const patchdl_asset_t* asset = patchdl_asset_find(url);

  if(!asset) {
    return queue_text(conn, MHD_HTTP_NOT_FOUND, "not found");
  }

  return queue_buffer(conn, MHD_HTTP_OK, asset->mime, asset->data,
                      asset->size);
}

static const char*
source_type_for_title(const char* title_id) {
  if(!title_id) {
    return "unknown";
  }

  if(!strcmp(title_id, "PPSA90001_00")) {
    return "shadowmount";
  }

  if(!strcmp(title_id, "PPSA08329_00")) {
    return "unknown";
  }

  return "official";
}

static int
parse_title_action(const char* url, char* title_id, size_t title_id_size,
                   char* action, size_t action_size) {
  const char* prefix = "/api/titles/";
  const char* start;
  const char* slash;
  size_t len;

  if(strncmp(url, prefix, strlen(prefix))) {
    return -1;
  }

  start = url + strlen(prefix);
  slash = strchr(start, '/');
  if(!slash || slash == start || !slash[1]) {
    return -1;
  }

  len = slash - start;
  if(len >= title_id_size) {
    return -1;
  }
  memcpy(title_id, start, len);
  title_id[len] = 0;

  len = strlen(slash + 1);
  if(len >= action_size) {
    return -1;
  }
  memcpy(action, slash + 1, len + 1);
  return 0;
}

static enum MHD_Result
handle_title_action(struct MHD_Connection* conn, const char* url) {
  char title_id[32];
  char action[24];
  const char* source_type;

  if(parse_title_action(url, title_id, sizeof(title_id), action,
                        sizeof(action))) {
    return queue_text(conn, MHD_HTTP_NOT_FOUND, "not found");
  }

  source_type = source_type_for_title(title_id);

  if(!strcmp(action, "download")) {
    if(!strcmp(source_type, "unknown")) {
      return queue_json(conn, MHD_HTTP_FORBIDDEN,
                        "{\"ok\":false,\"reason\":\"source_unknown\"}");
    }

    if(!strcmp(source_type, "shadowmount")) {
      return queue_json(conn, MHD_HTTP_ACCEPTED,
                        "{\"ok\":true,\"queued\":true,"
                        "\"source_type\":\"shadowmount\","
                        "\"install_allowed\":false}");
    }

    return queue_json(conn, MHD_HTTP_ACCEPTED,
                      "{\"ok\":true,\"queued\":true,"
                      "\"install_allowed\":true}");
  }

  if(!strcmp(action, "check")) {
    return queue_json(conn, MHD_HTTP_ACCEPTED,
                      "{\"ok\":true,\"queued\":true,\"action\":\"check\"}");
  }

  return queue_text(conn, MHD_HTTP_NOT_FOUND, "not found");
}

static enum MHD_Result
on_request(void* cls, struct MHD_Connection* conn, const char* url,
           const char* method, const char* version, const char* upload_data,
           size_t* upload_data_size, void** con_cls) {
  (void)cls;
  (void)version;
  (void)upload_data;
  (void)con_cls;

  if(!strcmp(method, MHD_HTTP_METHOD_OPTIONS)) {
    return queue_text(conn, MHD_HTTP_NO_CONTENT, "");
  }

  if(!strcmp(method, MHD_HTTP_METHOD_POST)) {
    if(*upload_data_size) {
      *upload_data_size = 0;
      return MHD_YES;
    }

    if(!strcmp(url, "/api/config")) {
      return queue_json(conn, MHD_HTTP_OK, config_json);
    }

    if(!strncmp(url, "/api/titles/", 12)) {
      return handle_title_action(conn, url);
    }

    return queue_text(conn, MHD_HTTP_NOT_FOUND, "not found");
  }

  if(strcmp(method, MHD_HTTP_METHOD_GET) &&
     strcmp(method, MHD_HTTP_METHOD_HEAD)) {
    return queue_text(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "method not allowed");
  }

  if(!strcmp(url, "/api/status")) {
    return queue_json(conn, MHD_HTTP_OK, status_json);
  }
  if(!strcmp(url, "/api/config")) {
    return queue_json(conn, MHD_HTTP_OK, config_json);
  }
  if(!strcmp(url, "/api/titles")) {
    return queue_json(conn, MHD_HTTP_OK, titles_json);
  }
  if(!strcmp(url, "/api/downloads")) {
    return queue_json(conn, MHD_HTTP_OK, downloads_json);
  }

  return queue_asset(conn, url);
}

int
patchdl_websrv_start(unsigned short port) {
  if(web_daemon) {
    return 0;
  }

  web_daemon = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD |
                                MHD_USE_THREAD_PER_CONNECTION,
                                port, NULL, NULL, &on_request, NULL,
                                MHD_OPTION_END);

  return web_daemon ? 0 : -1;
}

void
patchdl_websrv_stop(void) {
  if(web_daemon) {
    MHD_stop_daemon(web_daemon);
    web_daemon = 0;
  }
}
