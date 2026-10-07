# A man in the middle that only listens (README, "Someone listening in the middle").
# It accepts connections on one port, passes every byte on to another port in
# both directions, and writes a copy of everything it sees to a file.
#
#   python3 mitm_relay.py <listen_port> <real_port> <output_file>
import socket
import sys
import threading

listen_port, real_port = int(sys.argv[1]), int(sys.argv[2])
out = open(sys.argv[3], 'wb')

srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', listen_port))
srv.listen(8)
print('relay: listening on', listen_port, '-> forwarding to', real_port)


def pump(src, dst):
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            out.write(data)
            out.flush()
            dst.sendall(data)
    except OSError:
        pass
    for s in (src, dst):
        try:
            s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


while True:
    victim, _ = srv.accept()
    real = socket.create_connection(('127.0.0.1', real_port))
    threading.Thread(target=pump, args=(victim, real), daemon=True).start()
    threading.Thread(target=pump, args=(real, victim), daemon=True).start()
