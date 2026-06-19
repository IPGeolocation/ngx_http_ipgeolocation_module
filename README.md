# Nginx IP Geolocation Module for IPGeolocation.io


## Overview

Add country, city, ASN, company, and VPN/proxy/Tor security data to Nginx as native variables, powered by [IPGeolocation.io](https://ipgeolocation.io) MMDB databases. This Nginx IP geolocation module reads any IPGeolocation.io MaxMind DB (`.mmdb`) file directly from disk and exposes the data as `$ip_*` variables you can use anywhere in your `nginx.conf`, with no API calls, no network latency, and no per-request cost.

Use it to block anonymous traffic, redirect visitors by country, enforce geo-blocking and compliance rules, personalize content, and enrich your access logs, all at the edge before requests ever reach your application.

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
- [Configuration directives](#configuration-directives)
- [Variable reference](#variable-reference)
- [Real world examples](#real-world-examples)
- [How values are formatted](#how-values-are-formatted)
- [Client IP selection and X-Forwarded-For](#client-ip-selection-and-x-forwarded-for)
- [Getting the databases](#getting-the-databases)
- [Troubleshooting](#troubleshooting)
- [FAQ](#faq)
- [License](#license)

---

## Why use this module

Most teams reach for IP intelligence in one of two ways: calling a remote API on every request, or wiring a geolocation library into their application code. Both add latency, cost, and complexity. This module takes a third path. It loads IPGeolocation.io databases into Nginx memory once at startup and answers every lookup locally in microseconds.

- **No API calls and no per-request billing.** Lookups happen in process against a memory-mapped file. There is no outbound request, so there is nothing to rate limit and nothing to pay per lookup.
- **Decisions at the edge.** Block, redirect, or route traffic in `nginx.conf` before it reaches your backend. Your application never has to know the visitor's country or risk level unless you choose to forward it.
- **Load only what you need.** Point the module at a single database or at several. If you load only a Security database, only the security variables resolve. Load a full bundle and everything resolves. You pay in memory only for the data you actually use.
- **One module, many databases.** IP Location, IP Security, IP Company, IP to ASN, and IP Abuse Contact databases all work through the same directive and the same variable names.
- **Schema resilient.** Each variable knows several candidate field paths. The current nested IPGeolocation.io schema is tried first, the older flat schema second, so the same configuration keeps working across database versions.

---

## How it works

You load one or more `.mmdb` files with the `ipgeolocation_db` directive. The module opens each file at startup and keeps it memory mapped for the life of the worker process.

When Nginx evaluates an `$ip_*` variable during a request, the module:

1. Resolves the client IP address (see [Client IP selection](#client-ip-selection-and-x-forwarded-for)).
2. Looks that IP up in each loaded database, in the order you declared them.
3. Returns the first database that contains the requested field. This is why loading order is also priority order.
4. If no loaded database has the field, the variable resolves to an empty value.

Because the first match wins, you can layer databases. For example, load a Geolocation database and a Security database together, and `$ip_country_name` resolves from the first while `$ip_is_vpn` resolves from the second, automatically.

---

## Requirements

- **Nginx source** that matches the binary you intend to run. This module is compiled into Nginx, so you build Nginx with the module attached.
- **libmaxminddb** development headers and library. This is the official MaxMind DB reader that the module links against.
- A C compiler toolchain (`gcc` or `clang`, `make`).
- At least one IPGeolocation.io `.mmdb` database file. See [Getting the databases](#getting-the-databases).

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

The `mmdb-bin` package (Debian and Ubuntu) gives you `mmdblookup`, which is handy for inspecting a database and confirming which fields it contains.

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
sudo ./configure \
  --add-module=/path/to/nginx-ipgeolocation-module \
  --with-cc-opt='-I/usr/include -fPIC' \
  --with-ld-opt='-L/usr/lib -lmaxminddb'

sudo make
sudo make install
```

#### As a dynamic module (optional)

If you prefer to load the module at runtime instead of compiling it in, build it dynamically:

```bash
sudo ./configure --add-dynamic-module=/path/to/nginx-ipgeolocation-module --with-compat
sudo make modules
sudo cp objs/ngx_http_ipgeolocation_module.so /etc/nginx/modules/
```

Then load it at the very top of `nginx.conf`, before the `events` block:

```nginx
load_module modules/ngx_http_ipgeolocation_module.so;
```

### Verify the build

```bash
nginx -V 2>&1 | tr ' ' '\n' | grep -i ipgeolocation   # static build check
nginx -t                                               # config syntax check
```

---

## Quick start

Add the module configuration to the `http` block of your `nginx.conf`:

```nginx
http {
    # Load one or more IPGeolocation.io databases. Declaration order is priority order.
    ipgeolocation_db /etc/nginx/ipgeo/db-ip-geolocation.mmdb;
    ipgeolocation_db /etc/nginx/ipgeo/db-ip-security.mmdb;

    server {
        listen 80;
        server_name example.com;

        location / {
            # Pass enrichment to your backend as request headers
            proxy_set_header X-Geo-Country     $ip_country_code;
            proxy_set_header X-Geo-City         $ip_city_name;
            proxy_set_header X-Geo-Threat-Score $ip_threat_score;
            proxy_pass http://backend;
        }
    }
}
```

That is the whole setup. Every `$ip_*` variable in the [Variable reference](#variable-reference) is now available.

---

## Configuration directives

### `ipgeolocation_db`

```
Syntax:   ipgeolocation_db <path-to-mmdb-file>;
Context:  http
Default:  none
```

Loads an IPGeolocation.io MMDB file and keeps it in memory. **You can specify this directive more than once.** Each database is queried in the order it was declared, and the first database that contains a requested field wins. Load only the databases whose data you need.

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

```
Syntax:   ipgeolocation_trust_forwarded_header on | off;
Context:  http
Default:  on
```

Controls which IP address the module geolocates. When `on` (the default), the module uses the first address in the `X-Forwarded-For` header if that header is present, and falls back to the real connection peer address otherwise. When `off`, it always uses the real peer address.

Set this to `off` unless Nginx sits behind a proxy or load balancer that you control and that sets `X-Forwarded-For` for you, because clients can spoof that header. See [Client IP selection](#client-ip-selection-and-x-forwarded-for) for details.

```nginx
http {
    # Recommended when Nginx faces the public internet directly
    ipgeolocation_trust_forwarded_header off;
}
```

---

## Variable reference

Every variable below resolves only if a database you loaded contains the matching field. Variables backed by a database you did not load simply return an empty value, so it is safe to reference any of them.

The **Source database** column names the IPGeolocation.io product that supplies each field. A bundle that combines several products supplies all of their fields through the same variables.

### Location: country

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
| --- | --- | --- | --- |
| `$ip_state_code` | State or province code | `US-PA` |
| `$ip_state_name` | State or province name | `Pennsylvania` |
| `$ip_district_name` | District or county name | `Philadelphia County` |
| `$ip_city_name` | City name | `Philadelphia` |
| `$ip_city_name_en` | Legacy alias of `$ip_city_name` | `Philadelphia` |

### Location: postal, coordinates, time zone

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

| Variable | Description | Example | 
| --- | --- | --- | 
| `$ip_company_name` | Company or ISP name | `Tele2 Sverige AB` | 
| `$ip_company_domain` | Company domain | `tele2.com` | 
| `$ip_company_type` | Company type (for example ISP, HOSTING, BUSINESS) | `ISP` | 
| `$ip_isp_name` | ISP name (mapped onto company name) | `Tele2 Sverige AB` | 
| `$ip_organization_name` | Organization (falls back to ASN organization) | `Tele2 Sverige AB` |

### ASN

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

### Residential proxy

| Variable | Description | Example |
| --- | --- | -- | 
| `$ip_residential_proxy_provider_name` | Residential proxy provider name | Evomy Proxy
| `$ip_residential_proxy_last_seen` | Date last seen as a residential proxy | 2026-05-23

### Hosting

| Variable | Description | Example | 
| --- | --- | -- |
| `$ip_hosting_provider` | Hosting provider name | Amazon 

### Abuse contact

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

The snippets below assume the relevant database is loaded in your `http` block. Mix and match them freely.

### Block anonymous and malicious traffic (security)

Stop Tor exit nodes, known attackers, and high risk IPs before they reach your application. This is useful in front of login pages, checkout flows, signup forms, and admin panels.

```nginx
# Requires the IP Security database
map "$ip_is_tor$ip_is_known_attacker" $block_anon {
    default 0;
    "~1"    1;   # block if either flag is set
}

server {
    location /login {
        if ($block_anon) { return 403; }

        # Challenge or block anything with a high threat score
        if ($ip_threat_score ~ ^(8[0-9]|9[0-9]|100)$) { return 429; }

        proxy_pass http://app;
    }
}
```

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

server {
    location / {
        # Redirect to the regional site on first visit
        if ($http_x_redirected = "") {
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

Restrict access to specific countries for licensing, sanctions, or regulatory reasons. The example allows two countries and blocks the rest.

```nginx
# Requires the IP Geolocation database
map $ip_country_code $country_allowed {
    default 0;
    US      1;
    CA      1;
}

server {
    location / {
        if ($country_allowed = 0) { return 451; }   # 451: Unavailable For Legal Reasons
        proxy_pass http://app;
    }
}
```

### Separate humans from infrastructure (bot and datacenter filtering)

Cloud and hosting IPs rarely belong to real human visitors. Flag them so you can rate limit harder, skip ad rendering, or serve a lighter page.

```nginx
# Requires the IP Security database (cloud flag) or the IP to ASN database (ASN type)
map "$ip_is_cloud_provider$ip_asn_type" $is_infra {
    default              0;
    "~1"                 1;       # cloud provider flag set
    "~HOSTING"           1;       # ASN classified as hosting
}

server {
    location /api/ {
        limit_req_zone $binary_remote_addr zone=human:10m rate=30r/m;
        # Apply a stricter limit to infrastructure traffic in your own logic
        proxy_set_header X-Is-Infra $is_infra;
        proxy_pass http://api;
    }
}
```

### Enrich access logs for analytics and fraud review (observability)

Attach geolocation, network, and risk data to every log line. This makes fraud analysis, abuse triage, and traffic reporting far easier, with no changes to your application.

```nginx
log_format geo_enriched '$remote_addr - "$request" $status '
                        'country=$ip_country_code city="$ip_city_name" '
                        'asn=$ip_asn org="$ip_asn_organization" '
                        'vpn=$ip_is_vpn proxy=$ip_is_proxy tor=$ip_is_tor '
                        'threat=$ip_threat_score';

server {
    access_log /var/log/nginx/access.geo.log geo_enriched;
}
```

### Surface the abuse contact for an offending IP (incident response)

When you spot abuse in your logs, the abuse contact data tells you exactly where to send a report, without a manual WHOIS lookup.

```nginx
# Requires the IP Abuse Contact database
location = /debug/abuse {
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

---

## Client IP selection and X-Forwarded-For

The module geolocates a single IP address per request. Which one it uses depends on `ipgeolocation_trust_forwarded_header`.

- **Default (`on`):** if an `X-Forwarded-For` header is present, the module reads the first (leftmost) address in it, trimming surrounding spaces. If the header is absent, empty, or unusable, it falls back to the real connection peer address.
- **`off`:** the module always uses the real connection peer address and ignores `X-Forwarded-For` entirely.

`X-Forwarded-For` is set by clients and intermediaries and can be forged. Trust it only when Nginx sits behind a proxy or load balancer that you operate and that overwrites the header with the true client IP. If Nginx faces the public internet directly, set `ipgeolocation_trust_forwarded_header off;` so an attacker cannot change their apparent country or hide a flagged IP by sending a fake header.

---

## Getting the databases

This module reads IPGeolocation.io MMDB files. You download the databases from your IPGeolocation.io account and point the module at the files. The static download links you receive do not change, so the same paths work for your first download and for every update.

Relevant databases and their documentation:

- [IP Geolocation Database](https://ipgeolocation.io/ip-geolocation-database.html): country, state, city, postal code, coordinates, currency, and time zone. Available in [Geo Standard](https://ipgeolocation.io/geo-standard-databases.html) and [Geo Advance](https://ipgeolocation.io/geo-advance-databases.html) tiers (Advance adds accuracy radius and confidence).
- [IP Security Database](https://ipgeolocation.io/ip-security-database.html): threat score plus VPN, proxy, Tor, relay, bot, spam, attacker, and cloud provider flags. See the [database schema documentation](https://ipgeolocation.io/documentation/ip-security-database.html).
- [Residential Proxy Database](https://ipgeolocation.io/residential-proxy-database.html): residential proxy provider name and last seen date.
- [IP Hosting Database](https://ipgeolocation.io/ip-hosting-database.html): hosting provider name
- [IP Company Database](https://ipgeolocation.io/ip-company-database.html): company or ISP name, domain, and type. See the [database schema documentation](https://ipgeolocation.io/documentation/ip-company-database.html).
- [IP to ASN Database](https://ipgeolocation.io/ip-asn-database.html): AS number, organization, type, RIR, and allocation date. See the [database schema documentation](https://ipgeolocation.io/documentation/ip-asn-database-extended.html).
- [IP Abuse Contact Database](https://ipgeolocation.io/ip-abuse-contact-database.html): abuse email, phone, organization, route, and country. See the [database schema documentation](https://ipgeolocation.io/documentation/ip-abuse-contact-database.html).

You can buy a single database, a ready made bundle (for example IP to Location plus Company plus ASN plus Abuse), or a custom build. Compare options on [Database Pricing](https://ipgeolocation.io/db-pricing.html). 

After downloading, confirm which fields a file contains with `mmdblookup`:

```bash
mmdblookup --file /etc/nginx/ipgeo/db-ip-security.mmdb --ip 2.56.188.34
```

---

## Troubleshooting

**Nginx fails to start with "failed to open" for a database.**\

Check that the path in `ipgeolocation_db` is correct, the file exists, and the Nginx worker user can read it. The module logs the exact MaxMind error to help you diagnose it.

**A variable is always empty.**

Confirm you loaded a database that actually contains that field. Security variables need a Security database, ASN variables need an ASN database, and so on. Use `mmdblookup` to inspect the file. Also confirm the client IP is what you expect (see [Client IP selection](#client-ip-selection-and-x-forwarded-for)).

**The wrong country or city is returned for local traffic.**

Private and reserved IP ranges (for example `10.0.0.0/8` or `192.168.0.0/16`) are not in any public geolocation database, so they resolve to empty. This is expected for internal or test traffic.

**Visitors behind a proxy all look like one IP.**

If a trusted proxy sits in front of Nginx, make sure it sets `X-Forwarded-For` and keep `ipgeolocation_trust_forwarded_header on`. If Nginx is directly exposed, set it `off`.

**The build cannot find `maxminddb.h` or `-lmaxminddb`.**

Install the libmaxminddb development package (see [Requirements](#requirements)). The `config` file in [Installation](#installation-build-from-source) links the library automatically.

---

## Frequently Asked Questions

<details>
<summary><strong>Does this module call the IPGeolocation.io API?</strong></summary>
No. It reads local <code>.mmdb</code> database files and does not make any outbound requests. There are no per-request costs. To update the data, simply replace the database files on disk.
</details>

<details>
<summary><strong>How is this different from the Nginx GeoIP2 module?</strong></summary>
Both modules read MMDB files through `libmaxminddb`. However, this module is built specifically for IPGeolocation.io databases. It provides ready-to-use <code>$ip_*</code> variables for location, company, ASN, security, and abuse data, normalizes boolean values to <code>1</code> and <code>0</code>, joins provider lists, and automatically supports multiple schema versions without requiring manual <code>geoip2</code> field mappings.
</details>

<details>
<summary><strong>Can I use more than one database at the same time?</strong></summary>
Yes. You can declare <code>ipgeolocation_db</code> once for each database file. Databases are checked in declaration order, and the first one containing the requested field is used.
</details>

<details>
<summary><strong>Do I have to load every database?</strong></summary>
No. Load only the databases you need. Variables associated with databases that are not loaded simply return empty values, making it safe to reference them in your configuration.
</details>

<details>
<summary><strong>Does it support IPv4 and IPv6?</strong></summary>
Yes. IPGeolocation.io MMDB databases support both IPv4 and IPv6 addresses, and the module automatically performs lookups using whichever address the client provides.
</details>

<details>
<summary><strong>How do I update the data?</strong></summary>
Replace the existing <code>.mmdb</code> files on disk with the updated versions and reload Nginx. IPGeolocation.io publishes refreshed databases daily.
</details>

<details>
<summary><strong>Can I forward this data to my application?</strong></summary>
Yes. Use <code>proxy_set_header</code> to pass any <code>$ip_*</code> variable to your backend application, allowing it to receive geolocation and risk information without performing its own database lookups.
</details>

---

## License

See the [LICENSE](https://github.com/IPGeolocation/ngx_http_ipgeolocation_module/blob/main/LICENSE) file for details.

---

Built for [IPGeolocation.io](https://ipgeolocation.io) databases. Questions about the data, tiers, or bundles are answered on the [database documentation](https://ipgeolocation.io/documentation/databases.html) and [pricing](https://ipgeolocation.io/db-pricing.html) pages.