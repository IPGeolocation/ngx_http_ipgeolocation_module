/*
 * ngx_http_ipgeolocation_module.c
 *
 * Nginx module that exposes IPGeolocation.io MaxMind DB (.mmdb) data as
 * nginx variables.
 *
 * Design
 * ------
 *   - A single directive, `ipgeolocation_db`, points at any IPGeolocation.io
 *     MMDB file (IP Location, Security, Company, ASN, Abuse Contact, or any
 *     bundle that combines several of these).
 *
 *   - The directive may be specified MORE THAN ONCE. Every database is opened
 *     and kept in memory. When a variable is evaluated, each loaded database is
 *     queried in order and the first one that contains the requested field
 *     wins. This is what makes "fill the variable based on whichever database
 *     is loaded" work automatically -- load only a Security DB and only the
 *     security variables resolve; load a bundle and everything resolves.
 *
 *   - Each variable carries an ordered list of candidate field paths.
 *
 * Build (static, with the rest of nginx):
 *     ./configure --add-module=/path/to/this/dir \
 *                 --with-cc-opt='-I/usr/include -fPIC' \
 *                 --with-ld-opt='-L/usr/lib -lmaxminddb'
 *     make && make install
 *
 * Client IP selection:
 *     By default the first address in X-Forwarded-For is used when that header
 *     is present, otherwise the real connection peer address. Set
 *     `ipgeolocation_trust_forwarded_header off;` to always use the peer
 *     address (recommended unless nginx sits behind a trusted proxy that you
 *     control, because X-Forwarded-For can be spoofed by clients).
 */

 #include <ngx_config.h>
 #include <ngx_core.h>
 #include <ngx_http.h>
 
 #include <maxminddb.h>
 #include <stdio.h>
 
 
 #define NGX_IPGEO_MAX_CANDIDATES  4   /* candidate field paths per variable   */
 #define NGX_IPGEO_MAX_DEPTH       7   /* max path components incl. NULL term. */
 #define NGX_IPGEO_NUM_BUF         64  /* scratch buffer for numeric values    */
 
 
 typedef struct {
     ngx_array_t  *databases;   /* array of (MMDB_s *)                         */
     ngx_flag_t    trust_xff;   /* honour X-Forwarded-For when present         */
 } ngx_http_ipgeo_main_conf_t;
 
 
 /* One exported nginx variable and the field path(s) it maps to. */
 typedef struct {
     const char  *name;
     const char  *paths[NGX_IPGEO_MAX_CANDIDATES][NGX_IPGEO_MAX_DEPTH];
 } ngx_http_ipgeo_var_t;
 
 
 /*
  * Variable table.
  *
  * Each entry lists up to NGX_IPGEO_MAX_CANDIDATES candidate paths. Each path is
  * a NULL-terminated list of components. The current nested IPGeolocation.io
  * schema is listed first; legacy flat paths follow. Unused candidate slots are
  * left zero-initialised (their first component is NULL) and skipped.
  *
  */
 static ngx_http_ipgeo_var_t  ngx_http_ipgeo_vars[] = {
 
     /* ---- Location: country ---------------------------------------------- */
     { "ip_country_code",
         { {"location","country","code2",NULL}, {"country","code2",NULL}, {"country_code2",NULL} } },
     { "ip_country_code3",
         { {"location","country","code3",NULL}, {"country","code3",NULL}, {"country_code3",NULL} } },
     { "ip_country_code_ioc",
         { {"location","country","code_ioc",NULL}, {"country","code_ioc",NULL}, {"country_code_ioc",NULL} } },
     { "ip_country_name",
         { {"location","country","name","en",NULL}, {"country","name","en",NULL}, {"country_name","en",NULL} } },
     { "ip_country_name_en",   /* legacy alias of ip_country_name */
         { {"location","country","name","en",NULL}, {"country","name","en",NULL}, {"country_name","en",NULL} } },
     { "ip_country_name_official",
         { {"location","country","name_official","en",NULL}, {"country","name_official","en",NULL} } },
     { "ip_country_capital",
         { {"location","country","capital","en",NULL}, {"country","capital","en",NULL} } },
     { "ip_continent_code",
         { {"location","country","continent","code",NULL}, {"country","continent","code",NULL}, {"continent_code",NULL} } },
     { "ip_continent_name",
         { {"location","country","continent","name","en",NULL}, {"country","continent","name","en",NULL} } },
     { "ip_currency_code",
         { {"location","country","currency","code",NULL}, {"country","currency","code",NULL} } },
     { "ip_currency_name",
         { {"location","country","currency","name","en",NULL}, {"country","currency","name","en",NULL} } },
     { "ip_currency_symbol",
         { {"location","country","currency","symbol",NULL}, {"country","currency","symbol",NULL} } },
     { "ip_calling_code",
         { {"location","country","metadata","calling_code",NULL}, {"country","metadata","calling_code",NULL} } },
     { "ip_languages",
         { {"location","country","metadata","languages",NULL}, {"country","metadata","languages",NULL} } },
     { "ip_tld",
         { {"location","country","metadata","tld",NULL}, {"country","metadata","tld",NULL} } },
 
     /* ---- Location: state / district / city ------------------------------ */
     { "ip_state_code",
         { {"location","state","code",NULL} } },
     { "ip_state_name",
         { {"location","state","name","en",NULL} } },
     { "ip_district_name",
         { {"location","district","name","en",NULL} } },
     { "ip_city_name",
         { {"location","city","name","en",NULL}, {"city","en",NULL} } },
     { "ip_city_name_en",      /* legacy alias of ip_city_name */
         { {"location","city","name","en",NULL}, {"city","en",NULL} } },
 
     /* ---- Location: postal / coordinates / misc -------------------------- */
     { "ip_zip_code",
         { {"location","zipcode",NULL}, {"zip_code",NULL} } },
     { "ip_latitude",
         { {"location","coordinates","latitude",NULL}, {"latitude",NULL} } },
     { "ip_longitude",
         { {"location","coordinates","longitude",NULL}, {"longitude",NULL} } },
     { "ip_geoname_id",
         { {"location","geoname_id",NULL}, {"geo_name_id",NULL} } },
     { "ip_time_zone",
         { {"time_zone",NULL}, {"time_zone_name",NULL} } },
     /* Advance-tier extras (present only in Advance IP-to-Location data).
      * Verify exact paths against your file with `mmdbio inspect` and adjust. */
     { "ip_accuracy_radius",
         { {"location","accuracy_radius",NULL} } },
     { "ip_confidence",
         { {"location","confidence",NULL} } },
 
     /* ---- Company / ISP -------------------------------------------------- */
     { "ip_company_name",
         { {"company","name","en",NULL}, {"company","name",NULL},
           {"organization",NULL}, {"isp",NULL} } },
     { "ip_company_domain",
         { {"company","domain",NULL} } },
     { "ip_company_type",
         { {"company","type",NULL} } },
     { "ip_isp_name",          /* IPGeolocation maps ISP onto company name */
         { {"company","name","en",NULL}, {"company","name",NULL}, {"isp",NULL} } },
     { "ip_organization_name",
         { {"asn","organization",NULL}, {"company","name","en",NULL},
           {"organization",NULL} } },
 
     /* ---- ASN ------------------------------------------------------------ */
     { "ip_asn",
         { {"asn","as_number",NULL}, {"as_number",NULL}, {"asn",NULL} } },
     { "ip_asn_organization",
         { {"asn","organization",NULL}, {"as_organization",NULL} } },
     { "ip_asn_country",
         { {"asn","country_code",NULL}, {"asn","country",NULL}, {"as_country",NULL} } },
     { "ip_asn_domain",
         { {"asn","domain",NULL} } },
     { "ip_asn_type",
         { {"asn","type",NULL} } },
     { "ip_asn_rir",
         { {"asn","rir",NULL} } },
     { "ip_asn_date_allocated",
         { {"asn","date_allocated",NULL} } },
 
     /* ---- Security / threat ----------------------------------------------
      * Both nested (security.*) and flat (top-level) schemas are supported.
      * Stringified "true"/"false" booleans are normalised to 1/0 on output.
      * The *_provider variables resolve an array and join every entry with ", ".
      */
     { "ip_threat_score",
         { {"security","threat_score",NULL}, {"threat_score",NULL} } },
     { "ip_is_tor",
         { {"security","is_tor",NULL}, {"is_tor",NULL} } },
     { "ip_is_proxy",
         { {"security","is_proxy",NULL}, {"is_proxy",NULL} } },
     { "ip_is_vpn",
         { {"security","is_vpn",NULL}, {"is_vpn",NULL} } },
     { "ip_is_relay",
         { {"security","is_relay",NULL}, {"is_relay",NULL} } },
     { "ip_is_residential_proxy",
         { {"security","is_residential_proxy",NULL}, {"is_residential_proxy",NULL} } },
     { "ip_is_anonymous",
         { {"security","is_anonymous",NULL}, {"is_anonymous",NULL} } },
     { "ip_is_known_attacker",
         { {"security","is_known_attacker",NULL}, {"is_known_attacker",NULL} } },
     { "ip_is_bot",
         { {"security","is_bot",NULL}, {"is_bot",NULL} } },
     { "ip_is_spam",
         { {"security","is_spam",NULL}, {"is_spam",NULL} } },
     { "ip_is_cloud_provider",
         { {"security","is_cloud_provider",NULL}, {"is_cloud_provider",NULL} } },
     { "ip_cloud_provider",
         { {"security","cloud_provider_name",NULL}, {"cloud_provider_name",NULL} } },
     { "ip_proxy_provider",     /* array -> all names joined with ", " */
         { {"security","proxy_provider_names",NULL}, {"proxy_provider_names",NULL} } },
     { "ip_vpn_provider",       /* array -> all names joined with ", " */
         { {"security","vpn_provider_names",NULL}, {"vpn_provider_names",NULL} } },
     { "ip_relay_provider",
         { {"security","relay_provider_name",NULL}, {"relay_provider_name",NULL} } },
     { "ip_proxy_confidence",
         { {"security","proxy_confidence_score",NULL}, {"proxy_confidence_score",NULL} } },
     { "ip_vpn_confidence",
         { {"security","vpn_confidence_score",NULL}, {"vpn_confidence_score",NULL} } },
     { "ip_proxy_last_seen",
         { {"security","proxy_last_seen",NULL}, {"proxy_last_seen",NULL} } },
     { "ip_vpn_last_seen",
         { {"security","vpn_last_seen",NULL}, {"vpn_last_seen",NULL} } },
     { "ip_proxy_type",        /* legacy flat field */
         { {"security","proxy_type",NULL}, {"proxy_type",NULL} } },
 
     /* ---- Residential proxy ---------------------------------------------- */
     { "ip_residential_proxy_provider_name",
         { {"security","proxy_provider",NULL}, {"proxy_provider",NULL} } },
     { "ip_residential_proxy_last_seen",
         { {"security","last_seen",NULL}, {"last_seen",NULL} } },
 
     /* ---- Hosting provider ----------------------------------------------- */
     { "ip_hosting_provider",
         { {"security","hosting_provider",NULL}, {"hosting_provider",NULL} } },
 
     /* ---- Abuse contact -------------------------------------------------- */
     { "ip_abuse_name",
         { {"abuse","name","en",NULL}, {"abuse","name",NULL} } },
     { "ip_abuse_organization",
         { {"abuse","organization",NULL} } },
     { "ip_abuse_country_code",
         { {"abuse","country_code",NULL}, {"abuse","country",NULL} } },
     { "ip_abuse_address",
         { {"abuse","address",NULL} } },
     { "ip_abuse_email",
         { {"abuse","emails",NULL}, {"abuse","emails","0",NULL} } },
     { "ip_abuse_phone",
         { {"abuse","phone_numbers",NULL}, {"abuse","phone_numbers","0",NULL} } },
     { "ip_abuse_kind",
         { {"abuse","kind",NULL} } },
     { "ip_abuse_route",
         { {"abuse","route",NULL} } },
 };
 
 #define NGX_IPGEO_NVARS \
     (sizeof(ngx_http_ipgeo_vars) / sizeof(ngx_http_ipgeo_vars[0]))
 
 
 /* Forward declarations */
 static ngx_int_t ngx_http_ipgeo_variable(ngx_http_request_t *r,
     ngx_http_variable_value_t *v, uintptr_t data);
 static ngx_int_t ngx_http_ipgeo_get_client_ip(ngx_http_request_t *r,
     ngx_flag_t trust_xff, char *out, size_t out_size);
 static ngx_int_t ngx_http_ipgeo_set_value(ngx_http_request_t *r,
     ngx_http_variable_value_t *v, MMDB_entry_data_s *ed);
 static ngx_int_t ngx_http_ipgeo_set_array(ngx_http_request_t *r,
     ngx_http_variable_value_t *v, MMDB_entry_s *entry,
     const char *const *base);
 static ngx_int_t ngx_http_ipgeo_add_variables(ngx_conf_t *cf);
 static void *ngx_http_ipgeo_create_main_conf(ngx_conf_t *cf);
 static char *ngx_http_ipgeo_init_main_conf(ngx_conf_t *cf, void *conf);
 static char *ngx_http_ipgeo_db(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
 static void ngx_http_ipgeo_cleanup_db(void *data);
 
 
 static ngx_command_t  ngx_http_ipgeo_commands[] = {
 
     { ngx_string("ipgeolocation_db"),
       NGX_HTTP_MAIN_CONF | NGX_CONF_TAKE1,
       ngx_http_ipgeo_db,
       NGX_HTTP_MAIN_CONF_OFFSET,
       0,
       NULL },
 
     { ngx_string("ipgeolocation_trust_forwarded_header"),
       NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
       ngx_conf_set_flag_slot,
       NGX_HTTP_MAIN_CONF_OFFSET,
       offsetof(ngx_http_ipgeo_main_conf_t, trust_xff),
       NULL },
 
       ngx_null_command
 };
 
 
 static ngx_http_module_t  ngx_http_ipgeo_module_ctx = {
     ngx_http_ipgeo_add_variables,     /* preconfiguration  */
     NULL,                             /* postconfiguration */
     ngx_http_ipgeo_create_main_conf,  /* create main conf  */
     ngx_http_ipgeo_init_main_conf,    /* init main conf    */
     NULL,                             /* create srv conf   */
     NULL,                             /* merge srv conf    */
     NULL,                             /* create loc conf   */
     NULL                              /* merge loc conf    */
 };
 
 
 ngx_module_t  ngx_http_ipgeolocation_module = {
     NGX_MODULE_V1,
     &ngx_http_ipgeo_module_ctx,       /* module context    */
     ngx_http_ipgeo_commands,          /* module directives */
     NGX_HTTP_MODULE,                  /* module type       */
     NULL,                             /* init master       */
     NULL,                             /* init module       */
     NULL,                             /* init process      */
     NULL,                             /* init thread       */
     NULL,                             /* exit thread       */
     NULL,                             /* exit process      */
     NULL,                             /* exit master       */
     NGX_MODULE_V1_PADDING
 };
 
 
 /*
  * Resolve the client IP into a NUL-terminated string in `out`.
  * Returns NGX_OK on success, NGX_ERROR if no usable address was found.
  */
 static ngx_int_t
 ngx_http_ipgeo_get_client_ip(ngx_http_request_t *r, ngx_flag_t trust_xff,
     char *out, size_t out_size)
 {
     ngx_str_t         ip;
     ngx_table_elt_t  *xff;
 
     ip.len = 0;
     ip.data = NULL;
 
     if (trust_xff && r->headers_in.x_forwarded_for != NULL) {
         u_char  *p;
         size_t   len, i, start, end;
 
         xff = r->headers_in.x_forwarded_for;
         p = xff->value.data;
         len = xff->value.len;
 
         /* The left-most entry is the original client. Stop at the first comma. */
         for (i = 0; i < len && p[i] != ','; i++) { /* void */ }
 
         start = 0;
         while (start < i && (p[start] == ' ' || p[start] == '\t')) {
             start++;
         }
         end = i;
         while (end > start && (p[end - 1] == ' ' || p[end - 1] == '\t')) {
             end--;
         }
 
         if (end > start) {
             ip.data = p + start;
             ip.len = end - start;
         }
     }
 
     /* Fall back to the real peer address if XFF is absent/empty/oversized. */
     if (ip.len == 0 || ip.len >= out_size) {
         ip = r->connection->addr_text;
     }
 
     if (ip.len == 0 || ip.len >= out_size) {
         return NGX_ERROR;
     }
 
     ngx_memcpy(out, ip.data, ip.len);
     out[ip.len] = '\0';
 
     return NGX_OK;
 }
 
 
 /*
  * Format an MMDB entry into the nginx variable value (allocated from r->pool).
  * Booleans are rendered as "1"/"0" so they work directly in `if` tests.
  */
 static ngx_int_t
 ngx_http_ipgeo_set_value(ngx_http_request_t *r, ngx_http_variable_value_t *v,
     MMDB_entry_data_s *ed)
 {
     u_char  *value;
     char     nbuf[NGX_IPGEO_NUM_BUF];
     int      nlen;
     size_t   len;
 
     switch (ed->type) {
 
     case MMDB_DATA_TYPE_UTF8_STRING:
         len = ed->data_size;
 
         /* Normalize Boolean to these to 1/0 so the $ip_is_* variables are
          * consistent and usable in `if` tests regardless of how the database
          * encodes booleans. (A real text value is never exactly "true"/"false".) */
         if (len == 4 && ngx_strncmp(ed->utf8_string, "true", 4) == 0) {
             value = ngx_pnalloc(r->pool, 1);
             if (value == NULL) {
                 return NGX_ERROR;
             }
             value[0] = (u_char) '1';
             len = 1;
             break;
         }
         if (len == 5 && ngx_strncmp(ed->utf8_string, "false", 5) == 0) {
             value = ngx_pnalloc(r->pool, 1);
             if (value == NULL) {
                 return NGX_ERROR;
             }
             value[0] = (u_char) '0';
             len = 1;
             break;
         }
 
         value = ngx_pnalloc(r->pool, len);
         if (value == NULL) {
             return NGX_ERROR;
         }
         ngx_memcpy(value, ed->utf8_string, len);
         break;
 
     case MMDB_DATA_TYPE_BOOLEAN:
         value = ngx_pnalloc(r->pool, 1);
         if (value == NULL) {
             return NGX_ERROR;
         }
         value[0] = ed->boolean ? (u_char) '1' : (u_char) '0';
         len = 1;
         break;
 
     case MMDB_DATA_TYPE_UINT16:
         nlen = snprintf(nbuf, sizeof(nbuf), "%u", (unsigned) ed->uint16);
         goto numeric;
 
     case MMDB_DATA_TYPE_UINT32:
         nlen = snprintf(nbuf, sizeof(nbuf), "%u", (unsigned) ed->uint32);
         goto numeric;
 
     case MMDB_DATA_TYPE_INT32:
         nlen = snprintf(nbuf, sizeof(nbuf), "%d", ed->int32);
         goto numeric;
 
     case MMDB_DATA_TYPE_UINT64:
         nlen = snprintf(nbuf, sizeof(nbuf), "%llu",
                         (unsigned long long) ed->uint64);
         goto numeric;
 
     case MMDB_DATA_TYPE_DOUBLE:
         nlen = snprintf(nbuf, sizeof(nbuf), "%.6f", ed->double_value);
         goto numeric;
 
     case MMDB_DATA_TYPE_FLOAT:
         nlen = snprintf(nbuf, sizeof(nbuf), "%.6f", (double) ed->float_value);
         goto numeric;
 
     numeric:
         if (nlen < 0) {
             return NGX_ERROR;
         }
         if ((size_t) nlen >= sizeof(nbuf)) {
             nlen = sizeof(nbuf) - 1;
         }
         len = (size_t) nlen;
         value = ngx_pnalloc(r->pool, len);
         if (value == NULL) {
             return NGX_ERROR;
         }
         ngx_memcpy(value, nbuf, len);
         break;
 
     default:
         /* Maps, arrays, bytes and anything unexpected are not representable
          * as a scalar variable. */
         return NGX_DECLINED;
     }
 
     v->data = value;
     v->len = (unsigned) len;
     v->valid = 1;
     v->no_cacheable = 0;
     v->not_found = 0;
 
     return NGX_OK;
 }
 
 
 /*
  * Resolve an array field (e.g. proxy_provider_names) and join every string
  * element into a single ", "-separated value.
  * Returns NGX_DECLINED if the array holds no usable strings.
  */
 static ngx_int_t
 ngx_http_ipgeo_set_array(ngx_http_request_t *r, ngx_http_variable_value_t *v,
     MMDB_entry_s *entry, const char *const *base)
 {
     const char  *path[NGX_IPGEO_MAX_DEPTH + 1];
     char         idxbuf[16];
     ngx_str_t    items[32];
     u_char      *out, *p;
     size_t       total;
     ngx_uint_t   i, n, base_len;
 
     for (base_len = 0;
          base[base_len] != NULL && base_len < NGX_IPGEO_MAX_DEPTH;
          base_len++)
     {
         path[base_len] = base[base_len];
     }
 
     total = 0;
     n = 0;
 
     for (i = 0; i < (ngx_uint_t) (sizeof(items) / sizeof(items[0])); i++) {
         MMDB_entry_data_s  ed;
         int                status;
         u_char            *copy;
 
         (void) snprintf(idxbuf, sizeof(idxbuf), "%u", (unsigned) i);
         path[base_len] = idxbuf;
         path[base_len + 1] = NULL;
 
         status = MMDB_aget_value(entry, &ed, (const char *const *) path);
         if (status != MMDB_SUCCESS || !ed.has_data) {
             break;  /* past the end of the array */
         }
         if (ed.type != MMDB_DATA_TYPE_UTF8_STRING || ed.data_size == 0) {
             continue;  /* skip empty / non-string elements */
         }
 
         copy = ngx_pnalloc(r->pool, ed.data_size);
         if (copy == NULL) {
             return NGX_ERROR;
         }
         ngx_memcpy(copy, ed.utf8_string, ed.data_size);
 
         items[n].data = copy;
         items[n].len = ed.data_size;
         total += ed.data_size;
         n++;
     }
 
     if (n == 0) {
         return NGX_DECLINED;
     }
 
     if (n > 1) {
         total += (n - 1) * 2;   /* ", " between elements */
     }
 
     out = ngx_pnalloc(r->pool, total);
     if (out == NULL) {
         return NGX_ERROR;
     }
 
     p = out;
     for (i = 0; i < n; i++) {
         if (i > 0) {
             *p++ = ',';
             *p++ = ' ';
         }
         p = ngx_cpymem(p, items[i].data, items[i].len);
     }
 
     v->data = out;
     v->len = (unsigned) (p - out);
     v->valid = 1;
     v->no_cacheable = 0;
     v->not_found = 0;
 
     return NGX_OK;
 }
 
 
 static ngx_int_t
 ngx_http_ipgeo_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v,
     uintptr_t data)
 {
     ngx_http_ipgeo_main_conf_t  *mcf;
     ngx_http_ipgeo_var_t        *vd;
     MMDB_s                     **dbs;
     char                         ipstr[NGX_IPGEO_NUM_BUF];
     ngx_uint_t                   d, c;
 
     mcf = ngx_http_get_module_main_conf(r, ngx_http_ipgeolocation_module);
 
     if (mcf == NULL || mcf->databases == NULL || mcf->databases->nelts == 0) {
         v->not_found = 1;
         return NGX_OK;
     }
 
     if (ngx_http_ipgeo_get_client_ip(r, mcf->trust_xff, ipstr, sizeof(ipstr))
         != NGX_OK)
     {
         v->not_found = 1;
         return NGX_OK;
     }
 
     vd = &ngx_http_ipgeo_vars[(ngx_uint_t) data];
     dbs = mcf->databases->elts;
 
     for (d = 0; d < mcf->databases->nelts; d++) {
         int                    gai_err = 0, mmdb_err = 0;
         MMDB_lookup_result_s   result;
 
         result = MMDB_lookup_string(dbs[d], ipstr, &gai_err, &mmdb_err);
 
         if (gai_err != 0 || mmdb_err != MMDB_SUCCESS || !result.found_entry) {
             continue;
         }
 
         for (c = 0; c < NGX_IPGEO_MAX_CANDIDATES; c++) {
             MMDB_entry_data_s  ed;
             int                status;
             const char *const *path = (const char *const *) vd->paths[c];
 
             if (path[0] == NULL) {
                 break;  /* no further candidate paths for this variable */
             }
 
             status = MMDB_aget_value(&result.entry, &ed, path);
 
             if (status != MMDB_SUCCESS || !ed.has_data) {
                 continue;
             }
 
             if (ed.type == MMDB_DATA_TYPE_ARRAY) {
                 switch (ngx_http_ipgeo_set_array(r, v, &result.entry, path)) {
                 case NGX_OK:
                     return NGX_OK;          /* first hit wins */
                 case NGX_ERROR:
                     return NGX_ERROR;       /* allocation failure */
                 default:
                     continue;               /* empty array; try next candidate */
                 }
             }
 
             switch (ngx_http_ipgeo_set_value(r, v, &ed)) {
             case NGX_OK:
                 return NGX_OK;          /* first hit wins */
             case NGX_ERROR:
                 return NGX_ERROR;       /* allocation failure */
             default:
                 continue;               /* not scalar; try next candidate */
             }
         }
     }
 
     v->not_found = 1;
     return NGX_OK;
 }
 
 
 static ngx_int_t
 ngx_http_ipgeo_add_variables(ngx_conf_t *cf)
 {
     ngx_http_variable_t  *var;
     ngx_str_t             name;
     ngx_uint_t            i;
 
     for (i = 0; i < NGX_IPGEO_NVARS; i++) {
         name.data = (u_char *) ngx_http_ipgeo_vars[i].name;
         name.len = ngx_strlen(ngx_http_ipgeo_vars[i].name);
 
         var = ngx_http_add_variable(cf, &name, NGX_HTTP_VAR_NOCACHEABLE);
         if (var == NULL) {
             return NGX_ERROR;
         }
 
         var->get_handler = ngx_http_ipgeo_variable;
         var->data = (uintptr_t) i;
     }
 
     return NGX_OK;
 }
 
 
 static void *
 ngx_http_ipgeo_create_main_conf(ngx_conf_t *cf)
 {
     ngx_http_ipgeo_main_conf_t  *mcf;
 
     mcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_ipgeo_main_conf_t));
     if (mcf == NULL) {
         return NULL;
     }
 
     mcf->databases = ngx_array_create(cf->pool, 4, sizeof(MMDB_s *));
     if (mcf->databases == NULL) {
         return NULL;
     }
 
     mcf->trust_xff = NGX_CONF_UNSET;
 
     return mcf;
 }
 
 
 static char *
 ngx_http_ipgeo_init_main_conf(ngx_conf_t *cf, void *conf)
 {
     ngx_http_ipgeo_main_conf_t  *mcf = conf;
 
     /* Honour X-Forwarded-For by default (backward compatible). */
     ngx_conf_init_value(mcf->trust_xff, 1);
 
     return NGX_CONF_OK;
 }
 
 
 static void
 ngx_http_ipgeo_cleanup_db(void *data)
 {
     MMDB_s  *db = data;
 
     if (db != NULL) {
         MMDB_close(db);
     }
 }
 
 
 static char *
 ngx_http_ipgeo_db(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
 {
     ngx_http_ipgeo_main_conf_t  *mcf = conf;
     ngx_str_t                   *value;
     ngx_pool_cleanup_t          *cln;
     MMDB_s                      *db, **slot;
     int                          status;
 
     value = cf->args->elts;
 
     db = ngx_pcalloc(cf->pool, sizeof(MMDB_s));
     if (db == NULL) {
         return NGX_CONF_ERROR;
     }
 
     status = MMDB_open((const char *) value[1].data, MMDB_MODE_MMAP, db);
     if (status != MMDB_SUCCESS) {
         ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                            "ipgeolocation_db: failed to open \"%V\": %s",
                            &value[1], MMDB_strerror(status));
         return NGX_CONF_ERROR;
     }
 
     cln = ngx_pool_cleanup_add(cf->pool, 0);
     if (cln == NULL) {
         MMDB_close(db);
         return NGX_CONF_ERROR;
     }
     cln->handler = ngx_http_ipgeo_cleanup_db;
     cln->data = db;
 
     slot = ngx_array_push(mcf->databases);
     if (slot == NULL) {
         return NGX_CONF_ERROR;
     }
     *slot = db;
 
     ngx_conf_log_error(NGX_LOG_NOTICE, cf, 0,
                        "ipgeolocation_db: loaded \"%V\"", &value[1]);
 
     return NGX_CONF_OK;
 }