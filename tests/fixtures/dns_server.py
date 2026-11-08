"""Serve deterministic DNS packets and log fixture inputs; no test execution."""
import pathlib
import select
import socket
import struct
import sys
import time

root = pathlib.Path(sys.argv[1])
sockets = []
for family, host, filename in ((socket.AF_INET, '127.0.0.1', 'port'),
                               (socket.AF_INET6, '::1', 'port6')):
    udp = socket.socket(family, socket.SOCK_DGRAM)
    udp.bind((host, 0))
    tcp = socket.socket(family, socket.SOCK_STREAM)
    tcp.bind((host, udp.getsockname()[1]))
    tcp.listen()
    sockets += [udp, tcp]
    (root / filename).write_text(str(udp.getsockname()[1]))

def response(data, tcp, forged=False):
    offset, labels = 12, []
    while data[offset]:
        size = data[offset]
        labels.append(data[offset + 1:offset + 1 + size].decode().lower())
        offset += size + 1
    offset += 1
    kind = struct.unpack_from('!H', data, offset)[0]
    name = '.'.join(labels)
    if not forged:
        with (root / 'queries').open('a') as log:
            log.write(f'{name} {kind} {int(tcp)}\n')
    question = data[12:offset + 4]
    def packet(flags, count, records=b''):
        return data[:2] + struct.pack('!HHHHH', flags, 1, count, 0, 0) + question + records
    if name == 'silent.test':
        return None
    if name == 'broken.test':
        return packet(0x8180, 1, b'\xc0')
    if name == 'tcp.test' and not tcp:
        return packet(0x8380, 0)
    if name == 'cross.test' and not tcp and not forged:
        return packet(0x8380, 0) if kind == 1 else None
    if name not in ('dns.test', 'alias.test', 'target.test', 'tcp.test',
                    'short.suffix.test', 'wrong-port.test', 'cross.test'):
        return packet(0x8183, 0)
    owner, records, count = b'\xc0\x0c', b'', 1
    if name == 'alias.test':
        owner = b'\x06target\x04test\x00'
        records = b'\xc0\x0c' + struct.pack('!HHIH', 5, 1, 60, len(owner)) + owner
        count += 1
    suffix = '99' if forged else '51'
    address = socket.inet_pton(socket.AF_INET if kind == 1 else socket.AF_INET6,
                              '192.0.2.' + suffix if kind == 1 else '2001:db8::' + suffix)
    records += owner + struct.pack('!HHIH', kind, 1, 60, len(address)) + address
    return packet(0x8180, count, records)

clients = {}
cross_queries = {}
while True:
    for sock in select.select(sockets + list(clients), [], [])[0]:
        if sock in sockets:
            if sock.type == socket.SOCK_DGRAM:
                data, peer = sock.recvfrom(65535)
                if data[12:].startswith(b'\x05cross\x04test\x00'):
                    kind = struct.unpack_from('!H', data, 24)[0]
                    cross_queries[(sock.family, kind)] = (sock, peer, data)
                if data[12:].startswith(b'\x0awrong-port\x04test\x00'):
                    # Same address, wrong source port, followed by the real answer.
                    with socket.socket(sock.family, socket.SOCK_DGRAM) as other:
                        other.sendto(response(data, False, True), peer)
                    time.sleep(0.02)
                reply = response(data, False)
                if reply: sock.sendto(reply, peer)
            else:
                client, _ = sock.accept()
                clients[client] = b''
        else:
            chunk = sock.recv(65535)
            if not chunk:
                del clients[sock]
                sock.close()
                continue
            clients[sock] += chunk
            while len(clients[sock]) >= 2:
                length = struct.unpack('!H', clients[sock][:2])[0]
                if len(clients[sock]) < length + 2: break
                data, clients[sock] = clients[sock][2:length + 2], clients[sock][length + 2:]
                if data[12:].startswith(b'\x05cross\x04test\x00'):
                    # The A query moved to TCP. Answer it on the still-open UDP
                    # connection before sending its legitimate TCP answer.
                    udp, peer, _ = cross_queries[(sock.family, 1)]
                    udp.sendto(response(data, True, True), peer)
                    time.sleep(0.02)
                    udp6, peer6, data6 = cross_queries.pop((sock.family, 28))
                    udp6.sendto(response(data6, True), peer6)
                reply = response(data, True)
                if reply: sock.sendall(struct.pack('!H', len(reply)) + reply)
