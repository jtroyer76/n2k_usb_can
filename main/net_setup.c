#include "net_setup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_wifi.h"
#include "settings.h"

#define MAX_FORM  1024
#define PAGE_LEN  9216

static const char *TAG = "setup";

// Same look as the status page (web/index.html), light and dark.
static const char PAGE_HEAD[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>N2K CAN Gateway settings</title><style>"
    ":root{--bg:#f4f5f7;--card:#fff;--fg:#1c1e21;--dim:#65676b;--line:#dadde1;--ok:#1e8e3e;--bad:#d93025;--acc:#1a73e8}"
    "@media (prefers-color-scheme:dark){:root{--bg:#101214;--card:#1b1e22;--fg:#e4e6eb;--dim:#9aa0a6;--line:#2f3338;"
    "--ok:#3fb950;--bad:#f85149;--acc:#8ab4f8}}"
    "*{box-sizing:border-box}"
    "body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}"
    "header{display:flex;align-items:center;gap:12px;padding:12px 16px;border-bottom:1px solid var(--line);background:var(--card)}"
    "header h1{font-size:17px;margin:0;flex:1}header a{color:var(--acc)}"
    "main{padding:16px;max-width:520px;margin:0 auto}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px 14px;margin-bottom:12px}"
    "h2{font-size:15px;margin:0 0 4px}"
    "label{display:block;font-weight:600;margin:10px 0 4px}.hint{color:var(--dim);font-size:13px;font-weight:400}"
    "input{width:100%;font:inherit;padding:8px 10px;border-radius:6px;border:1px solid var(--line);background:var(--bg);color:var(--fg)}"
    "button{font:inherit;padding:8px 14px;border-radius:6px;border:1px solid var(--line);background:var(--card);color:var(--fg);cursor:pointer}"
    "button.primary{background:var(--acc);border-color:var(--acc);color:#fff;width:100%;margin-top:14px}"
    ".nets{display:flex;flex-wrap:wrap;gap:6px;margin-top:6px}.nets button{padding:5px 10px}"
    ".pw{font:16px ui-monospace,Consolas,Menlo,monospace;letter-spacing:.05em;margin:6px 0}"
    ".msg{display:none;padding:10px 12px;border-radius:6px;margin-top:12px}"
    ".msg.ok,.err{display:block;background:var(--ok);color:#fff;padding:10px 12px;border-radius:6px;margin-bottom:12px}"
    ".msg.bad,.err{background:var(--bad)}"
    "code{font-size:13px}"
    "</style></head><body>";

// Appends `s` to `out` with the characters HTML treats specially escaped.
static void put_escaped(char *out, size_t max, const char *s)
{
    size_t n = strlen(out);
    for (; *s && n + 7 < max; s++) {
        const char *e = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;"
                      : *s == '"' ? "&quot;" : *s == '\'' ? "&#39;" : NULL;
        if (e) {
            strcpy(out + n, e);
            n += strlen(e);
        } else {
            out[n++] = *s;
        }
    }
    out[n] = '\0';
}

static void put(char *out, size_t max, const char *s)
{
    size_t n = strlen(out);
    snprintf(out + n, max - n, "%s", s);
}

static esp_err_t send_page(httpd_req_t *req, const char *error)
{
    settings_t set;
    settings_get(&set);
    wifi_setup_t ws;
    net_wifi_setup_info(&ws);
    char (*nets)[33] = malloc(sizeof(char[33]) * SETUP_MAX_NETS);
    char *page = malloc(PAGE_LEN);
    if (!nets || !page) {
        free(nets);
        free(page);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    page[0] = '\0';
    put(page, PAGE_LEN, PAGE_HEAD);
    put(page, PAGE_LEN, "<header><h1>Settings</h1>");
    if (!ws.on) {   // on the hotspot there's no status page to go back to
        put(page, PAGE_LEN, "<a href=/>Status</a>");
    }
    put(page, PAGE_LEN, "</header><main>");
    if (error) {
        put(page, PAGE_LEN, "<div class=err>");
        put_escaped(page, PAGE_LEN, error);
        put(page, PAGE_LEN, "</div>");
    }

    // WiFi network
    put(page, PAGE_LEN, "<form method=post action=/setup id=f autocomplete=off><div class=card><h2>WiFi network</h2>");
    int n = net_wifi_setup_networks(nets, SETUP_MAX_NETS);
    if (n) {
        put(page, PAGE_LEN, "<div class=hint>In range (tap one):</div><div class=nets>");
        for (int i = 0; i < n; i++) {
            put(page, PAGE_LEN, "<button type=button onclick='f.ssid.value=this.textContent;f.wpass.focus()'>");
            put_escaped(page, PAGE_LEN, nets[i]);
            put(page, PAGE_LEN, "</button>");
        }
        put(page, PAGE_LEN, "</div>");
    }
    put(page, PAGE_LEN, "<label for=ssid>Network name <span class=hint>(case matters)</span></label>"
                        "<input id=ssid name=ssid required maxlength=32 autocapitalize=off autocorrect=off value=\"");
    put_escaped(page, PAGE_LEN, set.wifi_ssid);
    put(page, PAGE_LEN, "\"><label for=wpass>WiFi password <span class=hint>");
    put(page, PAGE_LEN, set.wifi_ssid[0] ? "(blank keeps the saved one for this network)" : "(blank for an open network)");
    put(page, PAGE_LEN, "</span></label><input id=wpass name=wpass type=password maxlength=63></div>");

    // Device password
    put(page, PAGE_LEN, "<div class=card><h2>Device password</h2><div class=hint>Protects firmware updates and these "
                        "settings. If the boat network goes missing, it's also the password of the gateway's own WiFi.</div>");
    if (!ws.have_saved) {   // first setup: the only time it's ever shown
        put(page, PAGE_LEN, "<div class=pw>");
        put_escaped(page, PAGE_LEN, set.password);
        put(page, PAGE_LEN, "</div><div class=hint>Note it down, or set your own:</div>");
    }
    put(page, PAGE_LEN, "<label for=newpw>New device password <span class=hint>(optional, 8&ndash;63 characters)</span></label>"
                        "<input id=newpw name=newpw maxlength=63 autocapitalize=off autocorrect=off>");
    if (ws.have_saved) {   // set up already: prove it's you
        put(page, PAGE_LEN, "<label for=curpw>Current device password</label><input id=curpw name=curpw type=password required>");
    }
    put(page, PAGE_LEN, "</div><button class=primary>Save and restart</button></form>");

    // Firmware update
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    put(page, PAGE_LEN, "<div class=card style='margin-top:12px'><h2>Firmware update</h2><div class=hint>Running <b>");
    put_escaped(page, PAGE_LEN, app->version);
    put(page, PAGE_LEN, "</b>");
    if (run) {
        put(page, PAGE_LEN, " (");
        put_escaped(page, PAGE_LEN, run->label);
        put(page, PAGE_LEN, ")");
    }
    put(page, PAGE_LEN, ". Choose a release's <code>-update.bin</code>. can0 drops while the gateway restarts. "
                        "If the new firmware doesn't get USB and CAN working within a minute or so, the gateway "
                        "goes back to this version by itself.</div>"
                        "<label for=fwfile>Firmware file</label><input type=file id=fwfile accept=.bin>"
                        "<label for=fwpass>Device password</label><input id=fwpass type=password>"
                        "<button type=button id=fwgo style='margin-top:10px'>Upload and restart</button>"
                        "<div id=fwmsg class=msg></div></div></main>"
                        "<script>"
                        "const $=id=>document.getElementById(id);"
                        "function msg(t,ok){const m=$('fwmsg');m.textContent=t;m.className='msg '+(ok?'ok':'bad');}"
                        "$('fwgo').onclick=async()=>{"
                        "const f=$('fwfile').files[0];if(!f){msg('Choose a firmware file first.',false);return;}"
                        "if(!confirm('Upload '+f.name+' and restart? can0 drops for a few seconds.'))return;"
                        "msg('Uploading\\u2026',true);"
                        "try{const r=await fetch('/update',{method:'POST',headers:{'X-OTA-Key':$('fwpass').value},body:f});"
                        "const t=await r.text();"
                        "if(!r.ok){msg(r.status===401?'Wrong device password.':(t||'Upload failed'),false);return;}"
                        "msg('Uploaded. The gateway is restarting with the new firmware.',true);"
                        "}catch(e){msg(\"Upload failed: can't reach the gateway.\",false);}};"
                        "</script></body></html>");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, page);
    free(page);
    free(nets);
    return err;
}

static esp_err_t page_get(httpd_req_t *req)
{
    return send_page(req, NULL);
}

// One form field, URL-decoded ("" if absent). False if it doesn't fit.
static bool form_field(const char *form, const char *key, char *out, size_t len)
{
    char raw[3 * 64 + 1];
    out[0] = '\0';
    esp_err_t err = httpd_query_key_value(form, key, raw, sizeof(raw));
    if (err == ESP_ERR_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        return false;
    }
    size_t n = 0;
    for (const char *p = raw; *p; p++) {
        if (n + 1 >= len) {
            return false;
        }
        if (*p == '+') {
            out[n++] = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            out[n++] = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            out[n++] = *p;
        }
    }
    out[n] = '\0';
    return true;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));   // let the reply go out
    esp_restart();
}

static esp_err_t setup_post(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= MAX_FORM) {
        return send_page(req, "Form too large");
    }
    char *form = malloc(req->content_len + 1);
    if (!form) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, form + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            free(form);
            return ESP_FAIL;
        }
        got += r;
    }
    form[got] = '\0';

    settings_t old, s;
    settings_get(&old);
    s = old;
    char wpass[SETTINGS_WIFI_PW_LEN], newpw[SETTINGS_PW_LEN], curpw[SETTINGS_PW_LEN];
    bool fits = form_field(form, "ssid", s.wifi_ssid, sizeof(s.wifi_ssid)) &&
                form_field(form, "wpass", wpass, sizeof(wpass)) &&
                form_field(form, "newpw", newpw, sizeof(newpw)) &&
                form_field(form, "curpw", curpw, sizeof(curpw));
    free(form);
    if (!fits) {
        return send_page(req, "Something in the form is too long");
    }

    wifi_setup_t ws;
    net_wifi_setup_info(&ws);
    if (ws.have_saved && !settings_password_ok(curpw)) {
        return send_page(req, "Wrong device password");
    }

    // A blank WiFi password keeps the saved one, for the same network.
    if (wpass[0] || strcmp(s.wifi_ssid, old.wifi_ssid) != 0) {
        snprintf(s.wifi_pass, sizeof(s.wifi_pass), "%s", wpass);
    }
    if (newpw[0]) {
        snprintf(s.password, sizeof(s.password), "%s", newpw);
    }

    const char *bad = settings_check(&s);
    if (bad) {
        return send_page(req, bad);
    }
    if (!settings_save(&s)) {
        return send_page(req, "Couldn't save the settings");
    }

    enum { SAVED_LEN = 4096 };
    char *page = malloc(SAVED_LEN);
    if (!page) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    page[0] = '\0';
    put(page, SAVED_LEN, PAGE_HEAD);
    put(page, SAVED_LEN, "<header><h1>Saved</h1></header><main><div class=card>"
                         "<p>The gateway is restarting and will join <b>");
    put_escaped(page, SAVED_LEN, s.wifi_ssid);
    put(page, SAVED_LEN, "</b>.</p><p>Put your phone back on that network. The gateway will be at <b>http://"
                         NET_HOSTNAME ".local/</b> (or its address from your router).</p>"
                         "<p class=hint>If it can't join that network, its own WiFi comes back after 5 minutes, "
                         "now protected by the device password.</p></div></main></body></html>");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, page);
    free(page);
    ESP_LOGI(TAG, "settings saved, restarting");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// The phone's "is there internet?" check, or a mistyped address: on the hotspot, go to the page.
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t code)
{
    wifi_setup_t ws;
    net_wifi_setup_info(&ws);
    if (!ws.on) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" SETUP_IP "/setup");
    return httpd_resp_send(req, NULL, 0);
}

void net_setup_register(httpd_handle_t server)
{
    static const httpd_uri_t page = { .uri = "/setup", .method = HTTP_GET, .handler = page_get };
    static const httpd_uri_t save = { .uri = "/setup", .method = HTTP_POST, .handler = setup_post };
    httpd_register_uri_handler(server, &page);
    httpd_register_uri_handler(server, &save);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
}
