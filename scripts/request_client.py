"""Small standard-library client for the bounded request protocol, not a load generator."""
import argparse
import json
import socket
import struct


def exact(connection, size):
    result = b''
    while len(result) < size:
        part = connection.recv(size-len(result))
        if not part:
            raise EOFError('Server closed before completing the response')
        result += part
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8081)
    parser.add_argument('--message', default='hello openEuler')
    parser.add_argument('--repeat', type=int, default=1)
    args = parser.parse_args()
    payload = args.message.encode('utf-8')
    if not 1 <= len(payload) <= 4096 or not 1 <= args.repeat <= 10000 or not 1 <= args.port <= 65535:
        parser.error('message must be 1..4096 UTF-8 bytes, repeat 1..10000 and port 1..65535')
    with socket.create_connection(('127.0.0.1', args.port), timeout=5) as connection:
        for _ in range(args.repeat):
            connection.sendall(struct.pack('!I', len(payload)) + payload)
            length, = struct.unpack('!I', exact(connection, 4))
            if length != len(payload):
                raise ValueError('Unexpected response length')
            response = exact(connection, length)
            if response != bytes(byte ^ 0x5a for byte in payload):
                raise ValueError('Response verification failed')
            print(json.dumps({'request_bytes': len(payload), 'verified': True, 'response_hex': response.hex()}))
        connection.shutdown(socket.SHUT_WR)
        if connection.recv(1):
            raise ValueError('Unexpected trailing response data')


if __name__ == '__main__':
    main()
