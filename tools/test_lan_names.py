"""Keep LAN peer naming checks independent of a particular household's addresses."""
import json
from pathlib import Path
import socket
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import verify


class LanNamesTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = Path(self.directory.name) / 'lan.json'
        self.hosts = dict(mac='mac-test.local', pc='pc-test.local', pi='pi-test.local')
        self.path.write_text(json.dumps({'hosts': self.hosts}))
        self.ctx = {'args': SimpleNamespace(lan_config=str(self.path), lan_only=True, lan_timeout=.2)}
        verify.results.clear()

    def tearDown(self):
        self.directory.cleanup()
        verify.results.clear()

    def test_all_named_peers_resolve(self):
        answer = [(socket.AF_INET, socket.SOCK_STREAM, 6, '', ('192.0.2.10', 0))]
        with patch('socket.getaddrinfo', return_value=answer):
            verify.check_lan_names(self.ctx)
        self.assertEqual([r[0] for r in verify.results], ['PASS'] * 3)

    def test_numeric_peer_is_rejected_before_network_access(self):
        self.hosts['pc'] = '192.0.2.10'
        self.path.write_text(json.dumps({'hosts': self.hosts}))
        with patch('socket.getaddrinfo') as resolver:
            verify.check_lan_names(self.ctx)
        resolver.assert_not_called()
        self.assertEqual(verify.results[0][0], 'FAIL')

    def test_dns_failure_and_loopback_only_do_not_claim_lan_connectivity(self):
        def resolve(host, *_args):
            if host == self.hosts['pc']:
                raise socket.gaierror('name unavailable')
            return [(socket.AF_INET, socket.SOCK_STREAM, 6, '', ('127.0.0.1', 0))]
        with patch('socket.getaddrinfo', side_effect=resolve):
            verify.check_lan_names(self.ctx)
        self.assertEqual([r[0] for r in verify.results], ['FAIL'] * 3)
    def test_stalled_resolver_does_not_block_the_verifier(self):
        release = threading.Event()
        self.ctx['args'].lan_timeout = .03
        def resolve(*_args):
            release.wait(1)
            return [(socket.AF_INET, socket.SOCK_STREAM, 6, '', ('192.0.2.10', 0))]
        try:
            with patch('socket.getaddrinfo', side_effect=resolve):
                started = time.monotonic()
                verify.check_lan_names(self.ctx)
                self.assertLess(time.monotonic() - started, .5)
            self.assertEqual([r[0] for r in verify.results], ['FAIL'] * 3)
            self.assertTrue(all('timeout' in r[2] for r in verify.results))
        finally:
            release.set()



class LanMdnsSingleAddressTest(LanNamesTest):
    def run_check(self, own, peer, wired=True):
        def fake(host, timeout):
            return (peer if host == self.hosts['pc'] else own), ''
        with patch.object(verify.os, 'name', 'posix'), patch('verify.windows_mdns_status', return_value=None), \
                patch('verify.linux_wired_up', return_value=wired), patch('verify.resolve_ipv4', side_effect=fake), \
                patch('socket.gethostname', return_value='this-host'):
            verify.check_lan_mdns_single_address(self.ctx)
        return {name: status for status, name, _ in verify.results}

    def test_single_addresses_pass(self):
        got = self.run_check(['192.168.1.5'], ['192.168.1.7'])
        self.assertEqual(got['LAN mDNS: one address'], 'PASS')
        self.assertEqual(got['LAN mDNS: pc name seen from here'], 'PASS')

    def test_two_lan_addresses_while_wired_warn(self):
        got = self.run_check(['192.168.1.5', '192.168.1.6'], ['192.168.1.7', '192.168.1.8'])
        self.assertEqual(got['LAN mDNS: one address'], 'WARN')
        self.assertEqual(got['LAN mDNS: pc name seen from here'], 'WARN')

    def test_loopback_and_link_local_ignored(self):
        got = self.run_check(['127.0.1.1', '169.254.3.4', '192.168.1.5'], ['192.168.1.7'])
        self.assertEqual(got['LAN mDNS: one address'], 'PASS')

    def test_windows_status_parse(self):
        out = ('Wired (802.3) adapters:\n  Ethernet [NIC] Up: 192.168.1.17/24\n'
               'Wireless (802.11) adapters:\n  Wi-Fi [WLAN] Up: 192.168.1.29/24\n'
               'Decision: block enabled (wired and wireless share a subnet: announce on wired only)\n'
               'Firewall group "KSPKontroller mDNS": 0 of 4 rules\n'
               'Task \\KSPKontroller\\mDNS prefer Ethernet: not installed\nIn sync: False\n')
        with patch.object(verify.os, 'name', 'nt'), patch('shutil.which', return_value='powershell'), \
                patch('subprocess.run', return_value=SimpleNamespace(stdout=out)):
            state = verify.windows_mdns_status()
        self.assertEqual(state['wired'], ['192.168.1.17'])
        self.assertEqual(state['wireless'], ['192.168.1.29'])
        self.assertEqual(state['rules'], (0, 4))
        self.assertTrue(state['block'])
        self.assertFalse(state['task'])


if __name__ == '__main__':
    unittest.main()
