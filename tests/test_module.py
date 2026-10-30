import argparse
import http.client
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest


FIXTURES = Path(__file__).resolve().parent / "fixtures"


class Nginx:
    binary = None
    module = None

    def __init__(self, trust=None, realip="", databases=None, ipv6=False):
        self.trust = trust
        self.realip = realip
        self.databases = databases if databases is not None else ["nested.mmdb"]
        self.host = "::1" if ipv6 else "127.0.0.1"
        self.family = socket.AF_INET6 if ipv6 else socket.AF_INET

    def __enter__(self):
        self.directory = tempfile.TemporaryDirectory(prefix="nginx-ipgeo-test-")
        self.root = Path(self.directory.name)
        self.process = None
        try:
            with socket.socket(self.family) as listener:
                listener.bind((self.host, 0))
                self.port = listener.getsockname()[1]
            address = f"[{self.host}]" if self.family == socket.AF_INET6 else self.host
            load = f'load_module "{self.module}";' if self.module else ""
            trust = (
                f"ipgeolocation_trust_forwarded_header {self.trust};"
                if self.trust is not None else ""
            )
            databases = "\n".join(
                f'ipgeolocation_db "{FIXTURES / name}";' for name in self.databases
            )
            (self.root / "nginx.conf").write_text(f"""
{load}
daemon off;
master_process off;
pid "{self.root / 'nginx.pid'}";
error_log "{self.root / 'error.log'}" notice;
events {{ worker_connections 64; }}
http {{
    access_log off;
    client_body_temp_path "{self.root / 'client-body'}";
    proxy_temp_path "{self.root / 'proxy'}";
    fastcgi_temp_path "{self.root / 'fastcgi'}";
    uwsgi_temp_path "{self.root / 'uwsgi'}";
    scgi_temp_path "{self.root / 'scgi'}";
    {databases}
    {trust}
    {self.realip}
    server {{
        listen {address}:{self.port};
        location = /inspect {{
            default_type application/json;
            return 200 '{{"ip":"$remote_addr","country":"$ip_country_code","vpn":"$ip_is_vpn","tor":"$ip_is_tor","score":"$ip_threat_score","asn":"$ip_asn","providers":"$ip_vpn_provider"}}';
        }}
        location = /restricted {{
            if ($ip_is_vpn) {{ return 403; }}
            return 200 "allowed";
        }}
    }}
}}
""")
            self.stderr = (self.root / "stderr.log").open("w")
            self.process = subprocess.Popen(
                [str(self.binary), "-p", str(self.root) + "/", "-c", "nginx.conf", "-e", "stderr"],
                stdout=subprocess.DEVNULL,
                stderr=self.stderr,
            )
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    raise RuntimeError((self.root / "stderr.log").read_text())
                try:
                    self.request()
                    return self
                except OSError:
                    time.sleep(0.025)
            raise RuntimeError("Nginx did not start within five seconds")
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *_):
        if self.process is not None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        if hasattr(self, "stderr"):
            self.stderr.close()
        self.directory.cleanup()

    def request(self, headers=(), path="/inspect"):
        connection = http.client.HTTPConnection(self.host, self.port, timeout=3)
        try:
            connection.putrequest("GET", path)
            for name, value in headers:
                connection.putheader(name, value)
            connection.endheaders()
            response = connection.getresponse()
            return response.status, response.read().decode()
        finally:
            connection.close()

    def inspect(self, forwarded=None, headers=None):
        if headers is None:
            headers = [] if forwarded is None else [("X-Forwarded-For", forwarded)]
        status, body = self.request(headers)
        if status != 200:
            raise AssertionError(f"Expected 200, received {status}: {body}")
        return json.loads(body)


class ModuleTests(unittest.TestCase):
    def test_default_ignores_supplied_headers_and_preserves_blocking(self):
        cases = [None, "203.0.113.42", "203.0.113.42, 198.51.100.42",
                 "2001:db8::42", "not-an-ip", "x" * 80]
        with Nginx() as server:
            for value in cases:
                with self.subTest(header=value):
                    data = server.inspect(value)
                    self.assertEqual(data["country"], "US")
                    self.assertEqual(data["vpn"], "1")
                    headers = [] if value is None else [("X-Forwarded-For", value)]
                    self.assertEqual(server.request(headers, "/restricted")[0], 403)
            data = server.inspect(headers=[("X-Forwarded-For", "203.0.113.42"),
                                           ("X-Forwarded-For", "198.51.100.42")])
            self.assertEqual(data["country"], "US")

    def test_explicit_off_ignores_supplied_header(self):
        with Nginx(trust="off") as server:
            self.assertEqual(server.inspect("203.0.113.42")["country"], "US")

    def test_explicit_on_preserves_ipv4_ipv6_and_first_address(self):
        with Nginx(trust="on") as server:
            for value, expected in [("203.0.113.42", "DE"),
                                    (" 203.0.113.42 \t, 198.51.100.42", "DE"),
                                    ("198.51.100.42", "FR"),
                                    ("2001:db8::42", "JP")]:
                with self.subTest(header=value):
                    self.assertEqual(server.inspect(value)["country"], expected)
            self.assertEqual(server.request([("X-Forwarded-For", "203.0.113.42")],
                                            "/restricted")[0], 200)

    def test_explicit_on_preserves_fallbacks(self):
        with Nginx(trust="on") as server:
            for value in [None, "", " \t ", "x" * 80]:
                with self.subTest(header=value):
                    self.assertEqual(server.inspect(value)["country"], "US")

    def test_explicit_on_invalid_ip_has_no_lookup_result(self):
        with Nginx(trust="on") as server:
            data = server.inspect("not-an-ip")
            self.assertEqual(data["country"], "")
            self.assertEqual(data["vpn"], "")

    def test_trusted_proxy_chain_uses_last_untrusted_address(self):
        realip = """
            set_real_ip_from 127.0.0.1/32;
            set_real_ip_from 10.0.0.3/32;
            real_ip_header X-Forwarded-For;
            real_ip_recursive on;
        """
        for trust in [None, "off"]:
            with self.subTest(trust=trust), Nginx(trust=trust, realip=realip) as server:
                data = server.inspect("203.0.113.42, 198.51.100.42, 10.0.0.3")
                self.assertEqual(data["ip"], "198.51.100.42")
                self.assertEqual(data["country"], "FR")
                data = server.inspect("2001:db8::42, 10.0.0.3")
                self.assertEqual(data["ip"], "2001:db8::42")
                self.assertEqual(data["country"], "JP")

    def test_untrusted_peer_cannot_replace_address_through_realip(self):
        realip = """
            set_real_ip_from 192.0.2.1/32;
            real_ip_header X-Forwarded-For;
            real_ip_recursive on;
        """
        with Nginx(realip=realip) as server:
            data = server.inspect("203.0.113.42")
            self.assertEqual(data["ip"], "127.0.0.1")
            self.assertEqual(data["country"], "US")

    def test_ipv6_connection_ignores_supplied_header(self):
        try:
            with socket.socket(socket.AF_INET6) as listener:
                listener.bind(("::1", 0))
        except OSError as error:
            self.skipTest(f"IPv6 loopback unavailable: {error}")
        with Nginx(ipv6=True) as server:
            data = server.inspect("203.0.113.42")
            self.assertEqual(data["ip"], "::1")
            self.assertEqual(data["country"], "US")

    def test_nested_database_formats(self):
        with Nginx() as server:
            data = server.inspect()
            self.assertEqual(data["country"], "US")
            self.assertEqual(data["vpn"], "1")
            self.assertEqual(data["tor"], "0")
            self.assertEqual(data["score"], "90")
            self.assertEqual(data["asn"], "64512")
            self.assertEqual(data["providers"], "Example One, Example Two")

    def test_legacy_database_formats(self):
        with Nginx(databases=["legacy.mmdb"]) as server:
            data = server.inspect()
            self.assertEqual(data["country"], "CA")
            self.assertEqual(data["vpn"], "1")
            self.assertEqual(data["tor"], "0")
            self.assertEqual(data["score"], "30")
            self.assertEqual(data["providers"], "Legacy One, Legacy Two")

    def test_database_declaration_order_is_preserved(self):
        for databases, expected in [(["nested.mmdb", "legacy.mmdb"], "US"),
                                    (["legacy.mmdb", "nested.mmdb"], "CA")]:
            with self.subTest(databases=databases), Nginx(databases=databases) as server:
                self.assertEqual(server.inspect()["country"], expected)

    def test_no_databases_produce_empty_values(self):
        with Nginx(databases=[]) as server:
            data = server.inspect()
            self.assertEqual(data["country"], "")
            self.assertEqual(data["vpn"], "")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--nginx", required=True, type=Path)
    parser.add_argument("--module", type=Path)
    arguments, remaining = parser.parse_known_args()
    Nginx.binary = arguments.nginx.resolve()
    Nginx.module = arguments.module.resolve() if arguments.module else None
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
