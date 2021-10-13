/* SPDX-License-Identifier: BSD-3-Clause */

#include <stdio.h>
#include <stdbool.h>
#include <time.h>

#include <uci.h>
#include <uci_blob.h>

#include <target.h>

#include <curl/curl.h>

#include "ovsdb.h"
#include "ovsdb_update.h"
#include "ovsdb_sync.h"
#include "ovsdb_table.h"
#include "ovsdb_cache.h"

#include "nl80211.h"
#include "radio.h"
#include "vif.h"
#include "phy.h"
#include "log.h"
#include "evsched.h"
#include "uci.h"
#include "utils.h"
#include "radius_proxy.h"

struct blob_buf uci_buf = {};
struct blob_attr *n;
extern ovsdb_table_t table_APC_State;
extern json_t* ovsdb_table_where(ovsdb_table_t *table, void *record);

enum {
	RADIUS_PROXY_OPTIONS_LISTEN_UDP,
	RADIUS_PROXY_OPTIONS_NAME,
	__RADIUS_PROXY_OPTIONS_MAX
};

enum {
	RADIUS_PROXY_CLIENT_NAME,
	RADIUS_PROXY_CLIENT_HOST,
	RADIUS_PROXY_CLIENT_TYPE,
	RADIUS_PROXY_CLIENT_SECRET,
	__RADIUS_PROXY_CLIENT_MAX
};

enum {
	RADIUS_PROXY_SERVER_NAME,
	RADIUS_PROXY_SERVER_HOST,
	RADIUS_PROXY_SERVER_TYPE,
	RADIUS_PROXY_SERVER_SECRET,
	RADIUS_PROXY_SERVER_PORT,
	RADIUS_PROXY_SERVER_STATUS,
	RADIUS_PROXY_SERVER_TLS,
	RADIUS_PROXY_SERVER_CERT_NAME_CHECK,
	RADIUS_PROXY_SERVER_DYNAMIC_LOOKUP,
	__RADIUS_PROXY_SERVER_MAX
};

enum {
	RADIUS_PROXY_TLS_NAME,
	RADIUS_PROXY_TLS_CA_CERT,
	RADIUS_PROXY_TLS_CLIENT_CERT,
	RADIUS_PROXY_TLS_CLIENT_KEY,
	RADIUS_PROXY_TLS_CERT_PASSWORD,
	__RADIUS_PROXY_TLS_MAX,
};

enum {
	RADIUS_PROXY_REALM_NAME,
	RADIUS_PROXY_REALM_AUTH_SERVER,
	RADIUS_PROXY_REALM_ACCT_SERVER,
	__RADIUS_PROXY_REALM_MAX
};


static const struct blobmsg_policy radius_proxy_options_policy[__RADIUS_PROXY_OPTIONS_MAX] = {
		[RADIUS_PROXY_OPTIONS_LISTEN_UDP] = { .name = "ListenUDP", BLOBMSG_TYPE_ARRAY },
		[RADIUS_PROXY_OPTIONS_NAME] = { .name = "name", BLOBMSG_TYPE_STRING },
};

static const struct blobmsg_policy radius_proxy_client_policy[__RADIUS_PROXY_CLIENT_MAX] = {
		[RADIUS_PROXY_CLIENT_NAME] = { .name = "name", BLOBMSG_TYPE_STRING },
		[RADIUS_PROXY_CLIENT_HOST] = { .name = "host", BLOBMSG_TYPE_STRING },
		[RADIUS_PROXY_CLIENT_TYPE] = { .name = "type", BLOBMSG_TYPE_STRING },
		[RADIUS_PROXY_CLIENT_SECRET] = { .name = "secret", BLOBMSG_TYPE_STRING },
};

static const struct blobmsg_policy radius_proxy_tls_policy[__RADIUS_PROXY_TLS_MAX] = {
		[RADIUS_PROXY_TLS_NAME] = { .name = "name", BLOBMSG_TYPE_STRING },
		[RADIUS_PROXY_TLS_CA_CERT] = { .name = "CACertificateFile", BLOBMSG_TYPE_STRING },
		[RADIUS_PROXY_TLS_CLIENT_CERT] = { .name = "certificateFile", BLOBMSG_TYPE_STRING },
		[RADIUS_PROXY_TLS_CLIENT_KEY] = { .name = "certificateKeyFile", BLOBMSG_TYPE_STRING },
		[RADIUS_PROXY_TLS_CERT_PASSWORD] = { .name = "certificateKeyPassword", BLOBMSG_TYPE_STRING },
};

static const struct blobmsg_policy radius_proxy_server_policy[__RADIUS_PROXY_SERVER_MAX] = {
		[RADIUS_PROXY_SERVER_NAME] = { .name = "name", BLOBMSG_TYPE_STRING },
		[RADIUS_PROXY_SERVER_HOST] = { .name = "host", BLOBMSG_TYPE_STRING },
		[RADIUS_PROXY_SERVER_TYPE] = { .name = "type", BLOBMSG_TYPE_STRING },
		[RADIUS_PROXY_SERVER_SECRET] = { .name = "secret", BLOBMSG_TYPE_STRING },
		[RADIUS_PROXY_SERVER_PORT] = { .name = "port", BLOBMSG_TYPE_INT32 },
		[RADIUS_PROXY_SERVER_STATUS] = { .name = "statusServer", BLOBMSG_TYPE_BOOL },
		[RADIUS_PROXY_SERVER_TLS] = { .name = "tls", BLOBMSG_TYPE_STRING },
		[RADIUS_PROXY_SERVER_CERT_NAME_CHECK] = { .name = "certificateNameCheck", BLOBMSG_TYPE_BOOL },
		[RADIUS_PROXY_SERVER_DYNAMIC_LOOKUP] = { .name = "dynamicLookupCommand", BLOBMSG_TYPE_STRING },
};

static const struct blobmsg_policy radius_proxy_realm_policy[__RADIUS_PROXY_REALM_MAX] = {
		[RADIUS_PROXY_REALM_NAME] = { .name = "name", BLOBMSG_TYPE_STRING },
		[RADIUS_PROXY_REALM_AUTH_SERVER] = { .name = "server", BLOBMSG_TYPE_ARRAY },
		[RADIUS_PROXY_REALM_ACCT_SERVER] = { .name = "accountingServer", BLOBMSG_TYPE_ARRAY },
};

const struct uci_blob_param_list radius_proxy_options_param = {
	.n_params = __RADIUS_PROXY_OPTIONS_MAX,
	.params = radius_proxy_options_policy,
};

const struct uci_blob_param_list radius_proxy_client_param = {
	.n_params = __RADIUS_PROXY_CLIENT_MAX,
	.params = radius_proxy_client_policy,
};

const struct uci_blob_param_list radius_proxy_tls_param = {
	.n_params = __RADIUS_PROXY_TLS_MAX,
	.params = radius_proxy_tls_policy,
};

const struct uci_blob_param_list radius_proxy_server_param = {
	.n_params = __RADIUS_PROXY_SERVER_MAX,
	.params = radius_proxy_server_policy,
};

const struct uci_blob_param_list radius_proxy_realm_param = {
	.n_params = __RADIUS_PROXY_REALM_MAX,
	.params = radius_proxy_realm_policy,
};


size_t file_write(void *ptr, size_t size, size_t nmemb, FILE *stream) {
	size_t written = fwrite(ptr, size, nmemb, stream);
	return written;
}

static bool radsec_download_cert(char *cert_name, char *dir_name, char *cert_url)
{
	CURL *curl;
	FILE *fp;
	CURLcode curl_ret;
	char path[200];
	char dir_path[200];
	char name[32];
	char dir[32];
	char *gw_clientcert = "/usr/opensync/certs/client.pem";
	char *gw_clientkey = "/usr/opensync/certs/client_dec.key";
	struct stat stat_buf;

	strcpy(name, cert_name);
	strcpy(dir, dir_name);
	sprintf(dir_path, "/tmp/radsec/certs/%s", dir);
	sprintf(path, "/tmp/radsec/certs/%s/%s", dir, name);

	if (stat(dir_path, &stat_buf) == -1)
	{
		char cmd[200];
		sprintf(cmd, "mkdir -p %s", dir_path);
		system(cmd);
	}

	curl = curl_easy_init();
	if (curl)
	{
		fp = fopen(path, "wb");

		if (fp == NULL)
		{
			curl_easy_cleanup(curl);
			return false;
		}

		if (cert_url == NULL)
		{
			curl_easy_cleanup(curl);
			fclose(fp);
			return false;
		}

		curl_easy_setopt(curl, CURLOPT_SSLCERT, gw_clientcert);
		curl_easy_setopt(curl, CURLOPT_SSLKEY, gw_clientkey);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
		curl_easy_setopt(curl, CURLOPT_HEADER, 0L);
		curl_easy_setopt(curl, CURLOPT_URL, cert_url);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_write);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
		curl_ret = curl_easy_perform(curl);

		if (curl_ret != CURLE_OK)
		{
			LOGE("radsec: certificate download failed %s", curl_easy_strerror(curl_ret));
			curl_easy_cleanup(curl);
			fclose(fp);
			remove(path);
			return false;
		}

		curl_easy_cleanup(curl);
		fclose(fp);
	}

	return true;
}

static bool radius_proxy_config_set(struct schema_Radius_Proxy_Config *conf)
{
	int i = 0;
	char path[200];
	char name[256];
	char server_name[256] = {};
	char acct_server_name[256] = {};
	char tls_name[256] = {};

	/* Configure options block */
	blob_buf_init(&uci_buf, 0);
	n = blobmsg_open_array(&uci_buf,"ListenUDP");
	blobmsg_add_string(&uci_buf, NULL, "*:1812");
	blobmsg_add_string(&uci_buf, NULL, "*:1813");
	blobmsg_close_array(&uci_buf, n);
	memset(name, '\0', sizeof(name));
	sprintf(name, "%s%s", conf->radius_config_name, "options");
	blobmsg_add_string(&uci_buf, "name", name);
	blob_to_uci_section(uci, "radsecproxy", name, "options",
			uci_buf.head, &radius_proxy_options_param, NULL);

	/* Configure client block */
	blob_buf_init(&uci_buf, 0);
	blobmsg_add_string(&uci_buf, "host", "0.0.0.0/0");
	blobmsg_add_string(&uci_buf, "type", "udp");
	blobmsg_add_string(&uci_buf, "secret", "secret");
	memset(name, '\0', sizeof(name));
	sprintf(name, "%s%s", conf->radius_config_name, "client");
	blobmsg_add_string(&uci_buf, "name", name);
	blob_to_uci_section(uci, "radsecproxy", name, "client",
			uci_buf.head, &radius_proxy_client_param, NULL);

	/* Configure TLS/non-TLS and server blocks */
	sprintf(server_name, "%s%s", conf->radius_config_name, "server");
	sprintf(acct_server_name, "%s%s", conf->radius_config_name, "Acctserver");
	sprintf(tls_name, "%s%s", conf->radius_config_name, "tls");
	if (conf->radsec)
	{
		blob_buf_init(&uci_buf, 0);
		radsec_download_cert("cacert.pem",
				conf->radius_config_name, conf->ca_cert);
		radsec_download_cert("clientcert.pem",
				conf->radius_config_name, conf->client_cert);
		radsec_download_cert("clientdec.key",
				conf->radius_config_name, conf->client_key);

		blobmsg_add_string(&uci_buf, "name", tls_name);

		memset(path, '\0', sizeof(path));
		sprintf(path, "/tmp/radsec/certs/%s/cacert.pem",
				conf->radius_config_name);
		blobmsg_add_string(&uci_buf, "CACertificateFile", path);

		memset(path, '\0', sizeof(path));
		sprintf(path, "/tmp/radsec/certs/%s/clientcert.pem",
				conf->radius_config_name);
		blobmsg_add_string(&uci_buf, "certificateFile", path);

		memset(path, '\0', sizeof(path));
		sprintf(path, "/tmp/radsec/certs/%s/clientdec.key",
				conf->radius_config_name);
		blobmsg_add_string(&uci_buf, "certificateKeyFile", path);

		if (strlen(conf->passphrase) > 0)
			blobmsg_add_string(&uci_buf, "certificateKeyPassword", conf->passphrase);

		blob_to_uci_section(uci, "radsecproxy", tls_name,
				"tls", uci_buf.head, &radius_proxy_tls_param, NULL);

		blob_buf_init(&uci_buf, 0);
		if (conf->auto_discover)
		{ /* auto discover radsec server address via realm DNS NAPTR record */
			blobmsg_add_string(&uci_buf, "dynamicLookupCommand", "/bin/dynamic_lookup.sh");
		}
		else
		{
			blobmsg_add_string(&uci_buf, "host", conf->server);
			blobmsg_add_u32(&uci_buf, "port", conf->port);
			blobmsg_add_string(&uci_buf, "secret", "radsec");
		}
		blobmsg_add_string(&uci_buf, "name", server_name);
		blobmsg_add_string(&uci_buf, "type", "tls");
		blobmsg_add_string(&uci_buf, "tls", tls_name);
		blobmsg_add_bool(&uci_buf, "statusServer", 0);
		blobmsg_add_bool(&uci_buf, "certificateNameCheck", 0);
		blob_to_uci_section(uci, "radsecproxy", server_name, "server",
				uci_buf.head, &radius_proxy_server_param, NULL);
	}
	else /* non-TLS block */
	{
		/* Authentication server */
		blob_buf_init(&uci_buf, 0);
		blobmsg_add_string(&uci_buf, "name", server_name);
		blobmsg_add_string(&uci_buf, "host", conf->server);
		blobmsg_add_string(&uci_buf, "type", "udp");
		if (strlen(conf->secret) > 0)
			blobmsg_add_string(&uci_buf, "secret", conf->secret);
		if (conf->port > 0)
			blobmsg_add_u32(&uci_buf, "port", conf->port);
		blob_to_uci_section(uci, "radsecproxy", server_name, "server",
				uci_buf.head, &radius_proxy_server_param, NULL);

		/* Accounting server */
		if (strlen(conf->acct_server) > 0)
		{
			blob_buf_init(&uci_buf, 0);
			blobmsg_add_string(&uci_buf, "name", acct_server_name);
			blobmsg_add_string(&uci_buf, "host", conf->acct_server);
			blobmsg_add_string(&uci_buf, "type", "udp");
			if (strlen(conf->secret) > 0)
				blobmsg_add_string(&uci_buf, "secret", conf->acct_secret);
			if (conf->acct_port > 0)
				blobmsg_add_u32(&uci_buf, "port", conf->acct_port);
			blob_to_uci_section(uci, "radsecproxy", acct_server_name, "server",
								uci_buf.head, &radius_proxy_server_param, NULL);
		}
	}

	/* Configure realm block */
	for (i = 0; i < conf->realm_len; i++)
	{
		blob_buf_init(&uci_buf, 0);
		blobmsg_add_string(&uci_buf, "name", conf->realm[i]);
		n = blobmsg_open_array(&uci_buf,"server");
		blobmsg_add_string(&uci_buf, NULL, server_name);
		blobmsg_close_array(&uci_buf, n);
		if (conf->radsec)
		{ /* Accounting server same as auth server */
			n = blobmsg_open_array(&uci_buf, "accountingServer");
			blobmsg_add_string(&uci_buf, NULL, server_name);
			blobmsg_close_array(&uci_buf, n);
		}
		else if (strlen(conf->acct_server) > 0)
		{ /* non-TLS case where accounting server is configured */
			n = blobmsg_open_array(&uci_buf, "accountingServer");
			blobmsg_add_string(&uci_buf, NULL, acct_server_name);
			blobmsg_close_array(&uci_buf, n);
		}
		memset(name, '\0', sizeof(name));
		sprintf(name, "%s%s%d", conf->radius_config_name, "realm", i);
		blob_to_uci_section(uci, "radsecproxy", name, "realm",
				uci_buf.head, &radius_proxy_realm_param, NULL);
	}
	uci_commit_all(uci);
	return true;
}

void radius_proxy_fixup()
{
	struct uci_package *radsecproxy;
	struct uci_context *proxy_ctx;
	struct uci_element *e = NULL, *tmp = NULL;
	int ret=0, wildcard_count=0, j=0, section_count=0;
	char *acct_server = NULL, *auth_server = NULL;
	struct uci_section *wildcard_section = NULL;
	struct blob_attr *tb[__RADIUS_PROXY_REALM_MAX] = {};
	struct blob_attr *n;
	char section_name[128] = {};

	if (!ovsdb_table_select_where(&table_Radius_Proxy_Config, NULL, &j))
		return;

	proxy_ctx = uci_alloc_context();
	ret = uci_load(proxy_ctx, "radsecproxy", &radsecproxy);
	if (ret) {
		LOGE("%s: uci_load() failed with rc %d", __func__, ret);
		if (proxy_ctx)
			uci_free_context(proxy_ctx);
		return;
	}

	uci_foreach_element_safe(&radsecproxy->sections, tmp, e) {
		struct uci_section *s = uci_to_section(e);
		if ((s == NULL) || (s->type == NULL)) continue;
		if (!strcmp(s->type, "realm")) {
			const char *realm_name;
			section_count++;
			realm_name = uci_lookup_option_string(proxy_ctx, s, "name");
			if (wildcard_count == 0 && (!strcmp((char *)realm_name, "*"))) {
				wildcard_section = s;
				wildcard_count = section_count;
			}
		}
		else {
			continue;
		}
	}

	TRACEF("wildcard_count: %d section_count: %d", wildcard_count, section_count);

	if ((wildcard_count != section_count) && (wildcard_section != NULL)) {
		blob_buf_init(&b, 0);
		uci_to_blob(&b, wildcard_section, &radius_proxy_realm_param);
		blobmsg_parse(radius_proxy_realm_policy, __RADIUS_PROXY_REALM_MAX, tb,
						blob_data(b.head), blob_len(b.head));

		/* Delete and re-add the wildcard section */
		struct blob_attr *ttb[1];
		blobmsg_parse_array(radius_proxy_realm_policy, ARRAY_SIZE(ttb), ttb,
							blobmsg_data(tb[RADIUS_PROXY_REALM_AUTH_SERVER]),
							blobmsg_data_len(tb[RADIUS_PROXY_REALM_AUTH_SERVER]));
		if (ttb[0]) {
			auth_server = blobmsg_get_string(ttb[0]);
		}

		blobmsg_parse_array(radius_proxy_realm_policy, ARRAY_SIZE(ttb), ttb,
							blobmsg_data(tb[RADIUS_PROXY_REALM_ACCT_SERVER]),
							blobmsg_data_len(tb[RADIUS_PROXY_REALM_ACCT_SERVER]));
		if (ttb[0]) {
			acct_server = blobmsg_get_string(ttb[0]);
		}

		strcpy(section_name, (char *)wildcard_section->e.name);

		TRACEF("Delete section: %s", section_name);

		uci_section_del(proxy_ctx, "radsecproxy", "radsecproxy",
							(char *)wildcard_section->e.name, "realm");

		blob_buf_init(&uci_buf, 0);
		blobmsg_add_string(&uci_buf, "name", "*");
		n = blobmsg_open_array(&uci_buf, "server");
		blobmsg_add_string(&uci_buf, NULL, auth_server);
		blobmsg_close_array(&uci_buf, n);
		n = blobmsg_open_array(&uci_buf, "accountingServer");
		blobmsg_add_string(&uci_buf, NULL, acct_server);
		blobmsg_close_array(&uci_buf, n);
		blob_to_uci_section(proxy_ctx, "radsecproxy", section_name, "realm",
				uci_buf.head, &radius_proxy_realm_param, NULL);
		TRACEF("Add section: %s %s %s", section_name, auth_server, acct_server);

		uci_commit_all(proxy_ctx);
	}
	uci_free_context(proxy_ctx);
}

static bool radius_proxy_config_delete()
{
	struct uci_package *radsecproxy;
	struct uci_context *rad_uci;
	struct uci_element *e = NULL, *tmp = NULL;
	int ret = 0;

	rad_uci = uci_alloc_context();

	ret = uci_load(rad_uci, "radsecproxy", &radsecproxy);
	if (ret) {
		LOGE("%s: uci_load() failed with rc %d", __func__, ret);
		uci_free_context(rad_uci);
		return false;
	}
	uci_foreach_element_safe(&radsecproxy->sections, tmp, e) {
		struct uci_section *s = uci_to_section(e);
		if ((s == NULL) || (s->type == NULL)) continue;
		uci_section_del(rad_uci, "radsecproxy", "radsecproxy",
				(char *)s->e.name, s->type);
	}
	uci_commit(rad_uci, &radsecproxy, false);
	uci_unload(rad_uci, radsecproxy);
	uci_free_context(rad_uci);
	return true;
}

void callback_Radius_Proxy_Config(ovsdb_update_monitor_t *self,
				 struct schema_Radius_Proxy_Config *old,
				 struct schema_Radius_Proxy_Config *conf)
{
	switch (self->mon_type)
	{
	case OVSDB_UPDATE_NEW:
	case OVSDB_UPDATE_MODIFY:
		(void) radius_proxy_config_set(conf);
		vif_check_radius_proxy();
		break;

	case OVSDB_UPDATE_DEL:
		(void) radius_proxy_config_delete();
		vif_check_radius_proxy();
		break;

	default:
		LOG(ERR, "Radius_Proxy_Config: unexpected mon_type %d %s",
				self->mon_type, self->mon_uuid);
		break;
	}
	set_config_apply_timeout(self, 0);
	return;
}


