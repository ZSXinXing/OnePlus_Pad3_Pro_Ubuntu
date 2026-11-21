import runpy
import socket
import tempfile
import threading
import unittest
from pathlib import Path


serve = runpy.run_path(Path(__file__).with_name('opd2513-torch'))['serve']


class TorchProtocolTest(unittest.TestCase):
    def exchange(self, command, led):
        client, server = socket.socketpair()
        with client, server:
            worker = threading.Thread(target=serve, args=(server, led))
            worker.start()
            client.settimeout(3)
            client.sendall(command)
            client.shutdown(socket.SHUT_WR)
            result = client.recv(3)
            worker.join(3)
            self.assertFalse(worker.is_alive())
            return result

    def test_round_trip_and_bad_value(self):
        with tempfile.TemporaryDirectory() as directory:
            led = Path(directory) / 'brightness'
            for value, brightness in ((b'1', b'26\n'), (b'0', b'0\n')):
                self.assertEqual(self.exchange(value, led), b'OK')
                self.assertEqual(led.read_bytes(), brightness)
            for invalid in (b'2', b'26', b'11', b'1\n', b''):
                self.assertEqual(self.exchange(invalid, led), b'ERR')
                self.assertEqual(led.read_bytes(), b'0\n')


if __name__ == '__main__':
    unittest.main()
