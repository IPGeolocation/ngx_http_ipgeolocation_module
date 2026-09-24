# Nginx IP Geolocation Module for IPGeolocation.io

## Overview

Add country, city, ASN, abuse contact, company, and [VPN, proxy and Tor detection data](https://ipgeolocation.io/ip-security-database.html) to Nginx as native variables, powered by [IPGeolocation.io](https://ipgeolocation.io) MMDB databases. This Nginx IP geolocation module reads any IPGeolocation.io DBs (`.mmdb`) file directly from disk and exposes the data as [`$ip_*` variables](#variable-reference) you can use anywhere in your `nginx.conf`, with no API calls, no network latency, and no per-request cost.

Use it to [block anonymous traffic](#block-anonymous-and-malicious-traffic-security), [redirect visitors by country](#redirect-and-personalize-by-country-geo-personalization), enforce [geo-blocking and compliance rules](#geo-blocking-and-compliance-access-control), personalize content, and [enrich your access logs](#enrich-access-logs-for-analytics-and-fraud-review-observability), all at the edge before requests ever reach your application.

```nginx
# Block Tor exit nodes and known attackers, redirect EU visitors, all at the edge
if ($ip_is_tor) { return 403; }
if ($ip_is_known_attacker) { return 403; }
if ($ip_country_code = DE) { return 302 https://de.example.com$request_uri; }
```

---

## Table of contents

- [Why use this module](#why-use-this-module)
- [How it works](#how-it-works)
- [Requirements](#requirements)
- [Installation (build from source)](#installation-build-from-source)
- [Quick start](#quick-start)
- [Testing your configuration](#testing-your-configuration)
- [Configuration directives](#configuration-directives)
- [Variable reference](#variable-reference)
- [Real world examples](#real-world-examples)
- [How values are formatted](#how-values-are-formatted)
- [Client IP selection and X-Forwarded-For](#client-ip-selection-and-x-forwarded-for)
- [Getting the databases](#getting-the-databases)
- [Updating the databases](#updating-the-databases)
- [Troubleshooting](#troubleshooting)
- [FAQ](#frequently-asked-questions)
- [License](#license)

---

## Why use this module

Most teams reach for IP intelligence in one of two ways: calling a remote API on every request, or wiring a geolocation library into their application code. Both add latency, cost, and complexity. This module takes a third path. It loads IPGeolocation.io databases into Nginx memory once at startup and answers every lookup locally in microseconds.

- **No API calls and no per-request billing.** Lookups happen in process against a memory-mapped file. There is no outbound request, so there is nothing to rate limit and nothing to pay per lookup.
- **Decisions at the edge.** Block, redirect, or route traffic in `nginx.conf` before it reaches your backend. Your application never has to know the visitor's country or risk level unless you choose to [forward it as request headers](#quick-start).
- **Load only what you need.** Point the module at a single database or at several. If you load only a Security database, only the security variables resolve. Load a full bundle and everything resolves. You pay in memory only for the data you actually use.
- **One module, many databases.** [IP Location](https://ipgeolocation.io/ip-geolocation-database.html), [IP Security](https://ipgeolocation.io/ip-security-database.html), [IP Company](https://ipgeolocation.io/ip-company-database.html), [IP to ASN](https://ipgeolocation.io/ip-asn-database.html), and [IP Abuse Contact](https://ipgeolocation.io/ip-abuse-contact-database.html) databases all work through the same directive and the same variable names.
- **Schema resilient.** Each variable knows several candidate field paths. The current nested IPGeolocation.io schema is tried first, the older flat schema second, so the same configuration keeps working across database versions.

---

## How it works

You load one or more `.mmdb` files with the [`ipgeolocation_db` directive](#ipgeolocation_db). The module opens each file at startup and keeps it memory mapped for the life of the worker process.

When Nginx evaluates an `$ip_*` variable during a request, the module:

1. Resolves the client IP address (see [how the client IP is selected](#client-ip-selection-and-x-forwarded-for)).
2. Looks that IP up in each loaded database, in the order you declared them.
3. Returns the first database that contains the requested field. This is why loading order is also priority order.
4. If no loaded database has the field, the variable resolves to an empty value.

Because the first match wins, you can layer databases. For example, load a Geolocation database and a Security database together, and `$ip_country_name` resolves from the first while `$ip_is_vpn` resolves from the second, automatically.

---

## Requirements

- **Nginx source** that matches the binary you intend to run. This module is compiled into Nginx, so you build Nginx with the module attached.
- **[libmaxminddb](https://github.com/maxmind/libmaxminddb)** development headers and library. This is the official MMDB reader that the module links against.
- A C compiler toolchain (`gcc` or `clang`, `make`).
- At least one IPGeolocation.io `.mmdb` database file. See [which databases provide which variables](#getting-the-databases).

Install libmaxminddb on common systems:

```bash
# Debian and Ubuntu
sudo apt-get update
sudo apt-get install -y libmaxminddb0 libmaxminddb-dev mmdb-bin

# RHEL, CentOS, Rocky, Alma (EPEL)
sudo yum install -y libmaxminddb libmaxminddb-devel

# macOS (Homebrew)
brew install libmaxminddb
```

The [mmdbio](https://github.com/IPGeolocation/mmdbio) package provided by IPGeolocation.io is handy for [inspecting a database and confirming which fields it contains](#getting-the-databases).

---

## Installation (build from source)

### Get the module source and config file

```bash
git clone https://github.com/IPGeolocation/ngx_http_ipgeolocation_module.git
```

The module directory must contain the C source and a standard Nginx `config` file. If your copy does not already include the `config` file, create one next to the `.c` source with this content:

```sh
ngx_addon_name=ngx_http_ipgeolocation_module

if test -n "$ngx_module_link"; then
    ngx_module_type=HTTP
    ngx_module_name=ngx_http_ipgeolocation_module
    ngx_module_srcs="$ngx_addon_dir/ngx_http_ipgeolocation_module.c"
    ngx_module_libs="-lmaxminddb"
    . auto/module
else
    HTTP_MODULES="$HTTP_MODULES ngx_http_ipgeolocation_module"
    NGX_ADDON_SRCS="$NGX_ADDON_SRCS $ngx_addon_dir/ngx_http_ipgeolocation_module.c"
    CORE_LIBS="$CORE_LIBS -lmaxminddb"
fi
```

This `config` file links `libmaxminddb` for you and supports both static and dynamic builds, so you do not have to pass the library by hand on the configure line.

### Get the matching Nginx source

Download the source tarball for your version from the [official Nginx download page](https://nginx.org/en/download.html):

```bash
# Replace x.y.z with the version that matches your installed Nginx
wget https://nginx.org/download/nginx-x.y.z.tar.gz
tar -xzf nginx-x.y.z.tar.gz
cd nginx-x.y.z
```

To find the version and build flags of an Nginx you already run, use `nginx -V`. Reuse those same `configure` arguments so your rebuilt binary stays compatible with your existing setup.

### Build as

#### A static module (compiled into Nginx)

```bash
./configure --add-module=/path/to/ngx_http_ipgeolocation_module
make
sudo make install
```

Only `make install` needs `sudo`. If libmaxminddb is installed outside the default search paths (for example Homebrew on Apple Silicon), point the compiler at it:

```bash
./configure --add-module=/path/to/ngx_http_ipgeolocation_module \
  --with-cc-opt='-I/opt/homebrew/include' \
  --with-ld-opt='-L/opt/homebrew/lib'
```

#### As a dynamic module (optional)

If you prefer to load the module at runtime instead of compiling it in, build it dynamically. The Nginx source version must exactly match the Nginx binary that will load the module, otherwise Nginx rejects it as "not binary compatible".

```bash
./configure --add-dynamic-module=/path/to/ngx_http_ipgeolocation_module --with-compat
make modules
sudo cp objs/ngx_http_ipgeolocation_module.so /etc/nginx/modules/
```

Then load it at the very top of `nginx.conf`, before the `events` block:

```nginx
load_module modules/ngx_http_ipgeolocation_module.so;
```

### Verify the build

```bash
# Static build: the module appears in the configure arguments
nginx -V 2>&1 | tr ' ' '\n' | grep -i ipgeolocation

# Dynamic build: confirm the load_module line is present, then check syntax
grep -n ipgeolocation /etc/nginx/nginx.conf

# Both: config syntax check
nginx -t
```

---

## Quick start

Add the module configuration to the `http` block of your `nginx.conf`:

```nginx
http {
    # Load one or more IPGeolocation.io databases. Declaration order is priority order.
    ipgeolocation_db /etc/nginx/ipgeo/db-ip-geolocation.mmdb;
    ipgeolocation_db /etc/nginx/ipgeo/db-ip-security.mmdb;

    # Nginx faces the internet directly, so geolocate the real peer address
    ipgeolocation_trust_forwarded_header off;

    server {
        listen 80;
        server_name example.com;

        location / {
            # Pass enrichment to your backend as request headers
            proxy_set_header X-Geo-Country      $ip_country_code;
            proxy_set_header X-Geo-City         $ip_city_name;
            proxy_set_header X-Geo-Threat-Score $ip_threat_score;
            proxy_pass http://backend;
        }
    }
}
```

That is the whole setup. Every variable in the [full `$ip_*` variable reference](#variable-reference) is now available. If Nginx runs behind a load balancer, read [how to select the correct client IP](#client-ip-selection-and-x-forwarded-for) before going live.

---

## Testing your configuration

Add a debug endpoint that prints the resolved values, restricted to internal addresses:

```nginx
# In the http block. Later examples reuse $is_internal.
geo $is_internal {
    default         0;
    127.0.0.1/32    1;
    ::1/128         1;
    10.0.0.0/8      1;
    172.16.0.0/12   1;
    192.168.0.0/16  1;
}

server {
    location = /debug/ipgeo {
        # `return` runs before allow/deny, so gate it with a variable instead
        if ($is_internal = 0) { return 403; }

        default_type text/plain;
        return 200 "country=$ip_country_code city=$ip_city_name asn=$ip_asn vpn=$ip_is_vpn tor=$ip_is_tor threat=$ip_threat_score\n";
    }
}
```

With `ipgeolocation_trust_forwarded_header on`, you can simulate any visitor from the server itself:

```bash
curl -H "X-Forwarded-For: 2.56.188.34" http://127.0.0.1/debug/ipgeo
```

Cross-check the output against the raw database record with `mmdbio read --db <db> --ip 2.56.188.34`.

---

## Configuration directives

### `ipgeolocation_db`

```
Syntax:   ipgeolocation_db <path-to-mmdb-file>;
Context:  http
Default:  none
```

Loads an IPGeolocation.io MMDB file and keeps it in memory. **You can specify this directive more than once.** Each database is queried in the order it was declared, and the first database that contains a requested field wins (see [how database priority order works](#how-it-works)). Load only the databases whose data you need.

```nginx
http {
    ipgeolocation_db /etc/nginx/ipgeo/db-ip-geolocation.mmdb;
    ipgeolocation_db /etc/nginx/ipgeo/db-ip-company.mmdb;
    ipgeolocation_db /etc/nginx/ipgeo/db-ip-asn.mmdb;
    ipgeolocation_db /etc/nginx/ipgeo/db-ip-security.mmdb;
    ipgeolocation_db /etc/nginx/ipgeo/db-ip-abuse-contact.mmdb;
}
```

If a database fails to open, Nginx refuses to start and logs the reason, so a bad path or a corrupt file is caught immediately rather than failing silently at request time.

### `ipgeolocation_trust_forwarded_header`

```bash
Syntax:   ipgeolocation_trust_forwarded_header on | off;
Context:  http
Default:  on
```

Controls which IP address the module geolocates. When `on` (the default), the module uses the first address in the `X-Forwarded-For` header if that header is present, and falls back to the real connection peer address otherwise. When `off`, it always uses the real peer address.

The directive is only valid in the `http` block, so one setting applies to every `server`. We recommend `off` for most deployments, because clients can spoof `X-Forwarded-For`. See [safe client IP selection behind proxies](#client-ip-selection-and-x-forwarded-for) for details.

```nginx
http {
    # Recommended for most deployments
    ipgeolocation_trust_forwarded_header off;
}
```

---

## Variable reference

Every variable below resolves only if a database you loaded contains the matching field. Variables backed by a database you did not load simply return an empty value, so it is safe to reference any of them. See [how booleans, lists, and empty values are formatted](#how-values-are-formatted).

### Location: country

Requires the [IP Geolocation database](https://ipgeolocation.io/ip-geolocation-database.html).

| Variable | Description | Example |
| --- | --- | --- |
| `$ip_country_code` | ISO 3166-1 alpha-2 country code | `US` |
| `$ip_country_code3` | ISO 3166-1 alpha-3 country code | `USA` |
| `$ip_country_code_ioc` | IOC country code | `USA` |
| `$ip_country_name` | Country name (English) | `United States` |
| `$ip_country_name_en` | Legacy alias of `$ip_country_name` | `United States` |
| `$ip_country_name_official` | Official country name | `United States of America` |
| `$ip_country_capital` | Capital city | `Washington, D.C.` |
| `$ip_continent_code` | Continent code | `NA` |
| `$ip_continent_name` | Continent name | `North America` |
| `$ip_currency_code` | ISO 4217 currency code | `USD` |
| `$ip_currency_name` | Currency name | `US Dollar` |
| `$ip_currency_symbol` | Currency symbol | `$` |
| `$ip_calling_code` | International dialing code | `+1` |
| `$ip_languages` | Languages for the country | `en-US,es-US` |
| `$ip_tld` | Country top level domain | `.us` |

### Location: state, district, city

| Variable | Description | Example |
| --- | --- | --- |
| `$ip_state_code` | State or province code | `US-PA` |
| `$ip_state_name` | State or province name | `Pennsylvania` |
| `$ip_district_name` | District or county name | `Philadelphia County` |
| `$ip_city_name` | City name | `Philadelphia` |
| `$ip_city_name_en` | Legacy alias of `$ip_city_name` | `Philadelphia` |

### Location: postal, coordinates, time zone

`$ip_accuracy_radius` and `$ip_confidence` are only available in the [Geo Advance database tier](https://ipgeolocation.io/geo-advance-databases.html).

| Variable | Description | Example |
| --- | --- | --- |
| `$ip_zip_code` | Postal or ZIP code | `19102` |
| `$ip_latitude` | Latitude in decimal degrees | `39.952580` |
| `$ip_longitude` | Longitude in decimal degrees | `-75.165220` |
| `$ip_geoname_id` | GeoNames place ID | `9849057` |
| `$ip_time_zone` | Time zone name | `America/New_York` |
| `$ip_accuracy_radius` | Accuracy radius in km (Advance tier) | `9.148` |
| `$ip_confidence` | Location confidence: low, medium, high (Advance tier) | `high` |

### Company and ISP

Requires the [IP Company database](https://ipgeolocation.io/ip-company-database.html). Field details are in the [IP Company database schema reference](https://ipgeolocation.io/documentation/ip-company-database.html#response-schema).

| Variable | Description | Example |
| --- | --- | --- |
| `$ip_company_name` | Company or ISP name | `Tele2 Sverige AB` |
| `$ip_company_domain` | Company domain | `tele2.com` |
| `$ip_company_type` | Company type (for example ISP, HOSTING, BUSINESS) | `ISP` |
| `$ip_isp_name` | ISP name (mapped onto company name) | `Tele2 Sverige AB` |
| `$ip_organization_name` | Organization (falls back to ASN organization) | `Tele2 Sverige AB` |

### ASN

Requires the [IP to ASN database](https://ipgeolocation.io/ip-asn-database.html). Field details are in the [IP to ASN extended database schema](https://ipgeolocation.io/documentation/ip-asn-database-extended.html#response-schema).

| Variable | Description | Example |
| --- | --- | --- |
| `$ip_asn` | Autonomous System Number | `AS1257` |
| `$ip_asn_organization` | ASN organization | `Tele2 Sverige AB` |
| `$ip_asn_country` | ASN country code | `SE` |
| `$ip_asn_domain` | ASN domain | `tele2.com` |
| `$ip_asn_type` | ASN type (for example ISP, HOSTING) | `ISP` |
| `$ip_asn_rir` | Regional Internet Registry | `RIPE` |
| `$ip_asn_date_allocated` | ASN allocation date | `2002-09-19` |

### Security and threat intelligence

Requires the [IP Security database](https://ipgeolocation.io/ip-security-database.html). Field details are in the [IP Security database fields and schema](https://ipgeolocation.io/documentation/ip-security-database.html#response-schema).

Boolean flags resolve to `1` or `0` so you can test them directly in `if` blocks and `map` directives. Provider lists (for example proxy and VPN provider names) are joined into a single comma separated value.

| Variable | Description | Example |
| --- | --- | --- |
| `$ip_threat_score` | Overall risk score, 0 to 100 | `80` |
| `$ip_is_tor` | 1 if the IP is a Tor exit node | `1` |
| `$ip_is_proxy` | 1 if the IP is a proxy | `1` |
| `$ip_is_vpn` | 1 if the IP is a VPN | `1` |
| `$ip_is_relay` | 1 if the IP is a relay (for example iCloud Private Relay) | `0` |
| `$ip_is_residential_proxy` | 1 if the IP is a residential proxy | `1` |
| `$ip_is_anonymous` | 1 if the IP is anonymized | `1` |
| `$ip_is_known_attacker` | 1 if the IP is a known attacker | `1` |
| `$ip_is_bot` | 1 if the IP is a known bot | `0` |
| `$ip_is_spam` | 1 if the IP is a known spam source | `0` |
| `$ip_is_cloud_provider` | 1 if the IP belongs to a cloud provider | `1` |
| `$ip_cloud_provider` | Cloud provider name | `Packethub S.A.` |
| `$ip_proxy_provider` | Proxy provider names, comma separated | `Zyte Proxy` |
| `$ip_vpn_provider` | VPN provider names, comma separated | `Nord VPN` |
| `$ip_relay_provider` | Relay provider name | |
| `$ip_proxy_confidence` | Proxy confidence score | `80` |
| `$ip_vpn_confidence` | VPN confidence score | `80` |
| `$ip_proxy_last_seen` | Date the IP was last seen as a proxy | `2025-12-12` |
| `$ip_vpn_last_seen` | Date the IP was last seen as a VPN | `2026-01-19` |
| `$ip_bot_confidence_score` | Confidence score for detected bot activity, 0 to 100 | `95` |
| `$ip_bot_operator_name` | Name of the bot operator, when available | `ChatGPT` |
| `$ip_bot_type` | Type of detected bot activity | `ai_crawler` |
| `$ip_is_known_good_bot` | 1 if the IP belongs to a known good bot, such as a search engine crawler | `1` |
| `$ip_bot_last_seen` | Date the IP was last seen exhibiting bot activity | `2026-09-03` |
| `$ip_is_corporate_gateway` | 1 if the IP belongs to a corporate gateway | `1` |
| `$ip_corporate_gateway_type` | Type of corporate gateway | `secure_web_gateway` |
| `$ip_corporate_gateway_provider_name` | Name of the corporate gateway provider | `Zscaler` |

### Residential proxy

Requires the [residential proxy detection database](https://ipgeolocation.io/residential-proxy-database.html). Field details are in the [Residential proxy database schema reference](https://ipgeolocation.io/documentation/residential-proxy-database.html#response-schema).

| Variable | Description | Example |
| --- | --- | --- |
| `$ip_residential_proxy_provider_name` | Residential proxy provider name | `Evomy Proxy` |
| `$ip_residential_proxy_last_seen` | Date last seen as a residential proxy | `2026-05-23` |

### Hosting

Requires the [IP hosting provider database](https://ipgeolocation.io/ip-hosting-database.html). Field details are in the [IP Hosting database schema reference](https://ipgeolocation.io/documentation/ip-hosting-database.html#response-schema).

| Variable | Description | Example |
| --- | --- | --- |
| `$ip_hosting_provider` | Hosting provider name | `Amazon` |

### Abuse contact

Requires the [IP Abuse Contact database](https://ipgeolocation.io/ip-abuse-contact-database.html). Field details are in the [IP Abuse Contact database field reference](https://ipgeolocation.io/documentation/ip-abuse-contact-database.html#response-schema).

| Variable | Description | Example |
| --- | --- | --- |
| `$ip_abuse_name` | Abuse contact name | `Swipnet Staff` |
| `$ip_abuse_country_code` | Abuse contact country | `SE` |
| `$ip_abuse_address` | Abuse contact postal address | |
| `$ip_abuse_email` | Abuse contact email | `abuse@tele2.com` |
| `$ip_abuse_phone` | Abuse contact phone | `+46 8 5626 42 10` |
| `$ip_abuse_kind` | Contact kind (for example group, person) | `group` |
| `$ip_abuse_route` | Network route or CIDR | `91.128.0.0/14` |

---

## Real world examples

The snippets below assume the relevant database is loaded in your `http` block (see [where to download each database](#getting-the-databases)). `map`, `geo`, `limit_req_zone`, and `log_format` belong in the `http` block; `if` and `return` go inside `server` or `location`. Mix and match them freely.

Using `if` only to `return` a response is safe. For anything more complex, compute a flag with [`map`](https://nginx.org/en/docs/http/ngx_http_map_module.html) and test that flag, as the examples below do.

### Block anonymous and malicious traffic (security)

Stop Tor exit nodes, known attackers, and high risk IPs before they reach your application. This is useful in front of login pages, checkout flows, signup forms, and admin panels.

```nginx
# Requires the IP Security database
map "$ip_is_tor$ip_is_known_attacker" $block_anon {
    default 0;
    "~1"    1;   # block if either flag is set
}

map $ip_threat_score $high_risk {
    default                  0;
    "~^(8[0-9]|9[0-9]|100)$" 1;   # threat score 80 to 100
}

server {
    location /login {
        if ($block_anon) { return 403; }
        if ($high_risk)  { return 429; }

        proxy_pass http://app;
    }
}
```

Avoid hard blocking `$ip_is_relay`, `$ip_is_anonymous`, or `$ip_is_corporate_gateway` on their own. iCloud Private Relay users and employees behind gateways such as Zscaler are usually legitimate customers, so challenge or rate limit them instead of returning 403.

### Redirect and personalize by country (geo personalization)

Send visitors to the right regional site, set the correct currency, or serve localized content.

```nginx
# Requires the IP Geolocation database
map $ip_country_code $regional_host {
    default  www.example.com;
    DE       de.example.com;
    FR       fr.example.com;
    JP       jp.example.com;
}

# Only the global site redirects, so regional sites can never loop
server {
    server_name www.example.com;

    location / {
        if ($regional_host != $host) {
            return 302 https://$regional_host$request_uri;
        }

        # Or pass locale hints to your app instead of redirecting
        proxy_set_header X-Geo-Country  $ip_country_code;
        proxy_set_header X-Geo-Currency $ip_currency_code;
        proxy_pass http://app;
    }
}
```

### Geo-blocking and compliance (access control)

Restrict access to specific countries for licensing, sanctions, or regulatory reasons. The example allows two countries, always allows internal traffic (health checks, monitoring), and blocks the rest. It reuses `$is_internal` from [Testing your configuration](#testing-your-configuration).

```nginx
# Requires the IP Geolocation database
map "$is_internal:$ip_country_code" $country_allowed {
    default        0;
    "~^1:"         1;   # private and internal addresses have no country
    "~:(US|CA)$"   1;   # allowed countries
}

server {
    location / {
        if ($country_allowed = 0) { return 451; }   # 451: Unavailable For Legal Reasons
        proxy_pass http://app;
    }
}
```

If search visibility matters, you can also allow verified crawlers with `$ip_is_known_good_bot` from the [IP Security database](https://ipgeolocation.io/ip-security-database.html).

### Separate humans from infrastructure (bot and datacenter filtering)

Cloud and hosting IPs rarely belong to real human visitors. Flag them so you can rate limit harder, skip ad rendering, or serve a lighter page.

```nginx
# Requires the IP Security database (cloud flag) or the IP to ASN database (ASN type)
map "$ip_is_cloud_provider$ip_asn_type" $is_infra {
    default     0;
    "~1"        1;   # cloud provider flag set
    "~HOSTING"  1;   # ASN classified as hosting
}

# Human traffic gets an empty key, and requests with an empty key are not limited
map $is_infra $infra_limit_key {
    0  "";
    1  $binary_remote_addr;
}

limit_req_zone $infra_limit_key zone=infra:10m rate=30r/m;

server {
    location /api/ {
        limit_req zone=infra burst=10 nodelay;
        proxy_set_header X-Is-Infra $is_infra;
        proxy_pass http://api;
    }
}
```

See the [Nginx limit_req module documentation](https://nginx.org/en/docs/http/ngx_http_limit_req_module.html) for burst and delay tuning.

### Enrich access logs for analytics and fraud review (observability)

Attach geolocation, network, and risk data to every log line. This makes fraud analysis, abuse triage, and traffic reporting far easier, with no changes to your application. `escape=json` keeps city and organization names with quotes or non-ASCII characters from breaking the log format.

```nginx
log_format geo_json escape=json '{'
    '"time":"$time_iso8601","ip":"$remote_addr","request":"$request","status":"$status",'
    '"country":"$ip_country_code","city":"$ip_city_name",'
    '"asn":"$ip_asn","org":"$ip_asn_organization",'
    '"vpn":"$ip_is_vpn","proxy":"$ip_is_proxy","tor":"$ip_is_tor",'
    '"threat":"$ip_threat_score"}';

server {
    access_log /var/log/nginx/access.geo.log geo_json;
}
```

IP addresses combined with city level location are personal data under regulations such as GDPR, so apply your normal log retention and access policies. More options are in the [Nginx log_format documentation](https://nginx.org/en/docs/http/ngx_http_log_module.html).

### Surface the abuse contact for an offending IP (incident response)

When you spot abuse in your logs, the abuse contact data tells you exactly where to send a report, without a manual WHOIS lookup. This is a debug endpoint: it is restricted to internal addresses, and values are not JSON escaped.

```nginx
# Requires the IP Abuse Contact database
location = /debug/abuse {
    if ($is_internal = 0) { return 403; }

    default_type application/json;
    return 200 '{"ip_route":"$ip_abuse_route","abuse_email":"$ip_abuse_email","abuse_phone":"$ip_abuse_phone","country":"$ip_abuse_country_code"}';
}
```

---

## How values are formatted

- **Booleans become `1` or `0`.** Whether the database stores a true boolean or the strings `"true"` and `"false"`, the module normalizes both to `1` and `0`. This means `$ip_is_vpn`, `$ip_is_tor`, and the other flags work directly in `if` tests and `map` blocks.
- **Lists are joined with `", "`.** Array fields such as proxy and VPN provider names are returned as a single comma separated string.
- **Numbers are rendered as text.** Scores and coordinates come back as plain strings, for example `80` or `-75.165220`, which is exactly what Nginx variables expect.
- **Empty when not found.** If no loaded database contains a field for the client IP, the variable resolves to an empty value. Reference any variable safely, even for databases you did not load.
- **Not cached per request.** These variables are evaluated each time they are used, so they always reflect the current request's client IP.

> [!TIP]
> Nginx treats both `0` and an empty value as false, so `if ($ip_is_tor)` fails open: if the Security database is missing or the IP is not in it, nothing is blocked. Allow lists work the opposite way, so in a `map` with `default 0`, an empty `$ip_country_code` is denied. Decide which behavior you want for unknown IPs and handle `""` explicitly.

---

## Client IP selection and X-Forwarded-For

The module geolocates a single IP address per request. Which one it uses depends on `ipgeolocation_trust_forwarded_header`.

- **Default (`on`):** if an `X-Forwarded-For` header is present, the module reads the first (leftmost) address in it, trimming surrounding spaces. If the header is absent, empty, or unusable, it falls back to the real connection peer address.
- **`off`:** the module always uses the real connection peer address and ignores `X-Forwarded-For` entirely.

> [!IMPORTANT]
> The leftmost `X-Forwarded-For` address is controlled by the client. Many load balancers and CDNs **append** the real IP to the header instead of replacing it, so a visitor can send a fake header to change their apparent country or hide a flagged IP, even behind a proxy. Keep `on` only if your proxy overwrites the header. Otherwise set `ipgeolocation_trust_forwarded_header off;` and use the [Nginx realip module](https://nginx.org/en/docs/http/ngx_http_realip_module.html) (`set_real_ip_from` plus `real_ip_header`), which replaces the peer address with the true client IP from trusted proxies only.

A safe setup behind a load balancer:

```nginx
http {
    ipgeolocation_trust_forwarded_header off;

    set_real_ip_from 10.0.0.0/8;          # your load balancer range
    real_ip_header   X-Forwarded-For;
    real_ip_recursive on;
}
```

---

## Getting the databases

This module reads IPGeolocation.io MMDB files. You download the databases from your IPGeolocation.io account and point the module at the files. The static download links you receive do not change, so the same paths work for your first download and for every update.

Relevant databases and their documentation:

- [IP Geolocation Database](https://ipgeolocation.io/ip-geolocation-database.html): country, state, city, postal code, coordinates, currency, and time zone. Available in [Geo Standard](https://ipgeolocation.io/geo-standard-databases.html) and [Geo Advance](https://ipgeolocation.io/geo-advance-databases.html) tiers (Advance adds accuracy radius and confidence).
- [IP Security Database](https://ipgeolocation.io/ip-security-database.html): threat score plus VPN, proxy, Tor, relay, bot, spam, attacker, corporate gateway, and cloud provider flags. See the [IP Security database fields and schema](https://ipgeolocation.io/documentation/ip-security-database.html).
- [Residential Proxy Database](https://ipgeolocation.io/residential-proxy-database.html): residential proxy provider name and last seen date.
- [IP Hosting Database](https://ipgeolocation.io/ip-hosting-database.html): hosting provider name.
- [IP Company Database](https://ipgeolocation.io/ip-company-database.html): company or ISP name, domain, and type. See the [IP Company database schema reference](https://ipgeolocation.io/documentation/ip-company-database.html).
- [IP to ASN Database](https://ipgeolocation.io/ip-asn-database.html): AS number, organization, type, RIR, and allocation date. See the [IP to ASN extended database schema](https://ipgeolocation.io/documentation/ip-asn-database-extended.html).
- [IP Abuse Contact Database](https://ipgeolocation.io/ip-abuse-contact-database.html): abuse email, phone, organization, route, and country. See the [IP Abuse Contact database field reference](https://ipgeolocation.io/documentation/ip-abuse-contact-database.html).

You can buy a single database, a ready made bundle (for example IP to Location plus Company plus ASN plus Abuse), or a custom build. [Compare IP database pricing and bundles](https://ipgeolocation.io/db-pricing.html).

After downloading, confirm which fields a file contains with [mmdbio](https://github.com/IPGeolocation/mmdbio):

```bash
mmdbio read --db /etc/nginx/ipgeo/db-ip-security.mmdb --ip 2.56.188.34
```

---

## Updating the databases

IPGeolocation.io publishes refreshed databases daily. Replace the files on disk, then reload Nginx so workers open the new versions.

> [!IMPORTANT]
> The module memory maps each database, so never overwrite a live `.mmdb` file in place (for example with `cp` or `wget -O`). Workers can read a half-written file and crash. Download to a temporary file in the **same directory**, then `mv` it over the old one, which is an atomic swap. Always run `nginx -t` before reloading so a corrupt download never takes the server down.

```bash
#!/bin/sh
set -e
DIR=/etc/nginx/ipgeo
URL="https://your-static-download-link"   # from your IPGeolocation.io account

curl -fsSL "$URL" -o "$DIR/db-ip-security.mmdb.tmp"
mv "$DIR/db-ip-security.mmdb.tmp" "$DIR/db-ip-security.mmdb"
nginx -t && nginx -s reload
```

Schedule it with cron or a systemd timer, for example daily at 04:00: `0 4 * * * /usr/local/bin/update-ipgeo.sh`.

---

## Troubleshooting

**Nginx fails to start with "failed to open" for a database.**

Check that the path in `ipgeolocation_db` is correct, the file exists, and the Nginx worker user can read it. The module logs the exact error to help you diagnose it.

**A variable is always empty.**

Confirm you loaded a database that actually contains that field. Security variables need a Security database, ASN variables need an ASN database, and so on (see [which databases provide each variable](#getting-the-databases)). Use `mmdbio read` to inspect the file. Also confirm the client IP is what you expect (see [how the client IP is selected](#client-ip-selection-and-x-forwarded-for)).

**Variables are empty for local or private IPs.**

Private and reserved IP ranges (for example `10.0.0.0/8` or `192.168.0.0/16`) are not in any public geolocation database, so they resolve to empty. This is expected for internal or test traffic. To test real lookups locally, follow [Testing your configuration](#testing-your-configuration).

**Visitors behind a proxy all look like one IP.**

Nginx is seeing the proxy's address. Use the [realip setup for load balancers](#client-ip-selection-and-x-forwarded-for) with `ipgeolocation_trust_forwarded_header off`, or keep it `on` only if your proxy overwrites `X-Forwarded-For`.

**Nginx reports the module "is not binary compatible".**

The dynamic module was built against a different Nginx version or without `--with-compat`. Rebuild it from the exact source version shown by `nginx -v` (see [building as a dynamic module](#as-a-dynamic-module-optional)).

**The build cannot find `maxminddb.h` or `-lmaxminddb`.**

Install the libmaxminddb development package (see [Requirements](#requirements)). The `config` file in [Installation](#installation-build-from-source) links the library automatically; for non-standard install paths, pass `--with-cc-opt` and `--with-ld-opt` as shown in [the static build step](#a-static-module-compiled-into-nginx).

---

## Frequently Asked Questions

<details> <summary><strong>Does this module call the IPGeolocation.io API?</strong></summary> No. It reads local `.mmdb` database files and does not make any outbound requests. There are no per-request costs. To update the data, [replace the database files safely](#updating-the-databases) and reload Nginx. </details>

<details> 
<summary><strong>How is this different from the Nginx GeoIP2 module?</strong></summary> Both modules read MMDB files through `libmaxminddb`. However, this module is built specifically for IPGeolocation.io databases. It provides [ready-to-use `$ip_*` variables](#variable-reference) for location, company, ASN, security, and abuse data, normalizes boolean values to `1` and `0`, joins provider lists, and automatically supports multiple schema versions without requiring manual `geoip2` field mappings.
</details>

<details> 
<summary><strong>Can I use more than one database at the same time?</strong></summary> Yes. You can declare `ipgeolocation_db` once for each database file. Databases are checked in declaration order, and the first one containing the requested field is used (see [how database priority order works](#how-it-works)).
</details>

<details> 
<summary><strong>Do I have to load every database?</strong></summary> No. Load only the databases you need. Variables associated with databases that are not loaded simply return empty values, making it safe to reference them in your configuration. 
</details>

<details> <summary><strong>Does it support IPv4 and IPv6?</strong></summary> Yes. IPGeolocation.io MMDB databases support both IPv4 and IPv6 addresses, and the module automatically performs lookups using whichever address the client provides. Make sure your `server` also listens on IPv6 (for example, `listen [::]:80;`).
</details>

<details> 
<summary><strong>How do I update the data?</strong></summary> IPGeolocation.io publishes refreshed databases daily. Download the new file, swap it in atomically with `mv`, and reload Nginx. See [update databases without downtime](#updating-the-databases) for a ready-to-use script. 
</details>

<details>
<summary><strong>Can I forward this data to my application?</strong></summary>
Yes. Use `proxy_set_header` to [pass geolocation headers to your backend](#quick-start) allowing it to receive geolocation and risk information without performing its own database lookups.
</details>

---

## License

See the [LICENSE](https://github.com/IPGeolocation/ngx_http_ipgeolocation_module/blob/main/LICENSE) file for details.

---

Built for [IPGeolocation.io](https://ipgeolocation.io) databases. Questions about the data, tiers, or bundles are answered in the [IPGeolocation.io database documentation](https://ipgeolocation.io/documentation/databases.html) and on the [IP database pricing page](https://ipgeolocation.io/db-pricing.html).
