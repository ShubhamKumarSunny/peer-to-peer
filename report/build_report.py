# Builds report/technical_report.html. Turn it into the PDF with Chrome:
#   python3 report/build_report.py
#   "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless --no-pdf-header-footer \
#       --print-to-pdf="Technical report.pdf" report/technical_report.html
import os
from diagrams import svg, text, box, arrow, path, seq, COL

HERE = os.path.dirname(os.path.abspath(__file__))


# ------------------------------------------------------------------ figures

def fig_architecture():
    b = ''
    b += box(140, 26, 190, 62, ['Tracker 1', 'port 4000'], 'tracker', size=13)
    b += box(430, 26, 190, 62, ['Tracker 2', 'port 4001'], 'tracker', size=13)
    b += arrow(334, 57, 426, 57, 'tracker', 'sync', both=True, ly=48)
    b += text(380, 76, 'OP n …', size=10.5, color=COL['tracker'][0], mono=True)
    b += box(40, 226, 170, 62, ['Alice', 'client  :5001'], 'client', size=13)
    b += box(295, 226, 170, 62, ['Bob', 'client  :5002'], 'client', size=13)
    b += box(550, 226, 170, 62, ['Carol', 'client  :5003'], 'client', size=13)
    b += arrow(125, 222, 205, 94, 'tracker')
    b += arrow(370, 222, 270, 94, 'tracker')
    b += arrow(635, 222, 545, 94, 'tracker')
    b += text(86, 150, 'commands and', size=11, color=COL['tracker'][0], weight='600')
    b += text(86, 165, 'answers', size=11, color=COL['tracker'][0], weight='600')
    b += text(86, 180, '(small text)', size=10.5, color=COL['tracker'][0])
    b += arrow(214, 257, 291, 257, 'data', both=True, width=2.2)
    b += arrow(469, 257, 546, 257, 'data', both=True, width=2.2)
    b += path('M 125 292 C 200 350, 560 350, 635 292', 'data', width=2.2)
    b += path('M 635 292 C 560 350, 200 350, 125 292', 'data', width=2.2)
    b += text(380, 318, 'file pieces travel directly between clients', size=11.5, color=COL['data'][0], weight='600')
    b += text(640, 150, 'The trackers never', size=11, color='#52606d', italic=True)
    b += text(640, 165, 'see file data.', size=11, color='#52606d', italic=True)
    return svg(760, 350, b)


def fig_pieces():
    b = ''
    b += text(20, 18, 'movie.bin — 1,300,000 bytes', size=12.5, anchor='start', weight='600')
    x0, y0, h = 20, 28, 40
    segs = [('piece 0', '524,288 bytes', 225), ('piece 1', '524,288 bytes', 225), ('piece 2', '251,424 bytes', 120)]
    x = x0
    centers = []
    for name, size, w in segs:
        b += box(x, y0, w, h, [name, size], 'data', size=11.5, r=3)
        centers.append(x + w / 2)
        x += w
    for i, c in enumerate(centers):
        b += arrow(c, y0 + h + 3, c, y0 + h + 24, 'plain', width=1.3)
        b += box(c - 58, y0 + h + 27, 116, 28, 'SHA-1 of piece %d' % i, 'sec', size=11, bold_first=False)
    b += arrow(x + 4, y0 + h / 2, 614, y0 + h / 2, 'plain', width=1.3)
    b += box(618, y0 + 2, 136, 36, ['SHA-1 of the', 'whole file'], 'sec', size=11, bold_first=False)
    yb = y0 + h + 27 + 28
    b += path('M 20 %d L 20 %d L 754 %d L 754 %d' % (yb + 4, yb + 12, yb + 12, y0 + 42), 'plain', end=False, width=1.1)
    b += arrow(387, yb + 12, 387, yb + 26, 'plain', width=1.3)
    b += box(110, yb + 29, 554, 30, 'sent to the tracker:  name, size, file hash, 3 piece hashes   (never the file itself)',
             'tracker', size=11.5, bold_first=False)
    return svg(774, yb + 66, b)


def fig_framing():
    b = ''
    mono = dict(size=11, bold_first=False)
    b += text(10, 20, 'Text messages: one line; a reply ends with a line END', size=12, anchor='start', weight='600')
    b += box(10, 30, 190, 30, 'LOGIN alice pw1  ⏎', 'client', **mono)
    b += arrow(206, 45, 262, 45, 'plain')
    b += box(268, 30, 180, 30, 'Login successful  ⏎', 'tracker', **mono)
    b += box(452, 30, 70, 30, 'END  ⏎', 'tracker', **mono)
    b += text(540, 49, 'the reader always reads up to END', size=10.5, anchor='start', color='#52606d', italic=True)

    b += text(10, 96, 'Piece data: text request, then 4 bytes of length, then exactly that many bytes', size=12,
              anchor='start', weight='600')
    b += box(10, 106, 230, 30, 'GET_PIECE movie.bin 2 movies  ⏎', 'client', **mono)
    b += arrow(246, 121, 282, 121, 'plain')
    b += box(288, 106, 110, 30, '00 03 D6 20', 'data', **mono)
    b += box(402, 106, 210, 30, '251,424 bytes of the piece', 'data', **mono)
    b += text(343, 152, 'length', size=10.5, color='#52606d', italic=True)
    b += text(507, 152, 'data (any bytes, even ⏎)', size=10.5, color='#52606d', italic=True)
    b += box(640, 106, 110, 30, 'FF FF FF FF', 'bad', **mono)
    b += text(695, 152, 'means "refused"', size=10.5, color=COL['bad'][0], italic=True)
    b += text(626, 125, 'or', size=11, color='#52606d')
    return svg(760, 162, b)


def fig_rarest():
    b = ''
    left, top, cw, ch = 150, 30, 78, 30
    peers = [('Alice', [1, 1, 1, 1, 1, 1]), ('Bob', [1, 1, 1, 1, 0, 0]), ('Carol', [1, 1, 0, 0, 0, 0])]
    for j in range(6):
        b += text(left + j * cw + cw / 2, top - 8, 'piece %d' % j, size=11.5, weight='600')
    for i, (name, have) in enumerate(peers):
        y = top + i * ch
        b += text(left - 14, y + 20, name + ' has', size=12, anchor='end', weight='600', color=COL['client'][0])
        for j, hv in enumerate(have):
            kind = 'client' if hv else 'white'
            b += box(left + j * cw + 3, y + 3, cw - 6, ch - 6, '✓' if hv else '–', kind, size=13, r=4)
    counts = [3, 3, 2, 2, 1, 1]
    y = top + 3 * ch + 8
    b += text(left - 14, y + 20, 'how many have it', size=12, anchor='end', weight='600')
    for j, c in enumerate(counts):
        kind = 'bad' if c == 1 else ('data' if c == 2 else 'plain')
        b += box(left + j * cw + 3, y + 3, cw - 6, ch - 6, str(c), kind, size=13, r=4)
    y2 = y + ch + 28
    b += text(left - 14, y2 + 20, 'download order', size=12, anchor='end', weight='600')
    order = [('4', 'bad'), ('5', 'bad'), ('3', 'data'), ('2', 'data'), ('0', 'plain'), ('1', 'plain')]
    for j, (p, kind) in enumerate(order):
        b += box(left + j * cw + 3, y2 + 3, cw - 6, ch - 6, 'piece ' + p, kind, size=11.5, r=4)
        if j < 5:
            b += arrow(left + (j + 1) * cw - 4, y2 + ch / 2, left + (j + 1) * cw + 4, y2 + ch / 2, 'plain', width=1.2)
    b += text(left + cw, y2 + ch + 16, 'rarest first', size=10.5, color=COL['bad'][0], weight='600')
    b += text(left + 3 * cw, y2 + ch + 16, 'then these', size=10.5, color=COL['data'][0], weight='600')
    b += text(left + 5 * cw, y2 + ch + 16, 'most common last', size=10.5, color='#52606d', weight='600')
    b += text(left + 6 * cw + 14, y2 + 14, 'equal counts:', size=10.5, anchor='start', color='#52606d', italic=True)
    b += text(left + 6 * cw + 14, y2 + 28, 'random order', size=10.5, anchor='start', color='#52606d', italic=True)
    return svg(760, 222, b)


def fig_piece_flow():
    b = ''
    x, w = 40, 330
    steps = [
        ('1. Take the next piece from the queue', 'under the download\'s lock', 'plain'),
        ('2. Choose a peer', 'has the piece, is alive, has the fewest open requests', 'plain'),
        ('3. Connect, TLS handshake', 'the peer must prove the key the tracker named', 'sec'),
        ('4. Ask for the piece; read length, then data', '5-second limit on every step', 'client'),
        ('5. Check: right length?  SHA-1 equals the tracker\'s hash?', '', 'sec'),
        ('6. Write it at its place in the file', 'pwrite at  index × 512KB;  mark the piece as held', 'data'),
        ('7. Put myself back at the end of the pool\'s queue', 'so other downloads get a turn', 'plain'),
    ]
    y = 12
    ys = []
    for i, (a, s, kind) in enumerate(steps):
        h = 40 if s else 32
        b += box(x, y, w, h, [a, s] if s else [a], kind, size=11.5)
        ys.append((y, h))
        if i < len(steps) - 1:
            b += arrow(x + w / 2, y + h + 1, x + w / 2, y + h + 12, 'plain', width=1.3)
        y += h + 14
    total = y
    # failure branch
    fy = ys[4][0]
    b += box(455, fy - 40, 290, 140, [], 'bad')
    b += text(600, fy - 18, 'If anything in steps 3–5 fails', size=11.5, weight='600')
    for i, line in enumerate(['• do not ask this peer for this piece again',
                              '• count one failure for the peer;',
                              '   3 in a row → the peer is dropped',
                              '• the piece goes back in the queue',
                              '• if nobody has it right now: wait 1 s and',
                              '   ask every peer again what it has']):
        b += text(470, fy + 2 + i * 16, line, size=11, anchor='start', color='#3e4c59')
    b += arrow(x + w + 3, fy + 18, 452, fy + 18, 'bad', 'no', ly=fy + 12)
    b += text(x + w / 2 + 14, ys[4][0] + ys[4][1] + 13, 'yes', size=10.5, anchor='start', color=COL['client'][0], weight='600')
    b += path('M 600 %d L 600 %d L %d %d' % (fy + 103, ys[6][0] + 20, x + w + 5, ys[6][0] + 20), 'bad')
    b += path('M %d %d L 14 %d L 14 %d L %d %d' % (x - 2, ys[6][0] + 23, ys[6][0] + 23, ys[0][0] + 23, x - 4, ys[0][0] + 23),
              'plain', dash=True)
    b += text(9, (ys[0][0] + ys[6][0]) / 2 + 23, 'repeat until the queue is empty', size=10.5, color='#52606d',
              italic=True, rotate=-90)
    b += box(455, 12, 290, 56, ['4 of these chains run per file', 'on a shared pool of 8 threads,',
                                'so 4 pieces are in flight at once'], 'plain', size=11)
    return svg(760, total, b)


def fig_threads():
    b = ''
    b += text(190, 18, 'TRACKER process', size=12, weight='700', color=COL['tracker'][0])
    b += box(20, 28, 160, 40, ['main thread', 'keyboard: quit'], 'tracker', size=11)
    b += box(200, 28, 160, 40, ['accept thread', 'new connections'], 'tracker', size=11)
    b += box(20, 84, 160, 40, ['connector thread', 'link to other tracker'], 'tracker', size=11)
    b += box(200, 84, 160, 40, ['one thread per client', 'read, run, answer'], 'tracker', size=11)
    b += box(60, 142, 260, 40, ['all data + oplog', 'one mutex: state_lock'], 'plain', size=11, dash=True)
    b += arrow(100, 126, 130, 140, 'plain', width=1.1)
    b += arrow(280, 126, 250, 140, 'plain', width=1.1)

    b += '<line x1="385" y1="8" x2="385" y2="190" stroke="#d5dae1" stroke-width="1"/>'
    b += text(575, 18, 'CLIENT process', size=12, weight='700', color=COL['client'][0])
    b += box(405, 28, 160, 40, ['shell thread', 'your commands'], 'client', size=11)
    b += box(585, 28, 160, 40, ['seeder accept thread', 'other peers connect'], 'client', size=11)
    b += box(405, 84, 160, 40, ['download pool', '8 worker threads'], 'data', size=11)
    b += box(585, 84, 160, 40, ['seeder pool', '8 worker threads'], 'data', size=11)
    b += arrow(665, 70, 665, 82, 'plain', width=1.1)
    b += arrow(485, 70, 485, 82, 'plain', width=1.1)
    b += box(405, 142, 340, 40, ['one TLS connection to the tracker', 'shared by all threads, one mutex'], 'plain',
             size=11, dash=True)
    b += arrow(485, 126, 500, 140, 'plain', width=1.1)
    b += arrow(665, 126, 650, 140, 'plain', width=1.1)
    return svg(760, 194, b)


def fig_oplog():
    b = ''
    rows = [('1', '1', 'CREATE_USER alice pbkdf2-sha256$600000$…'), ('1', '2', 'LOGIN alice'),
            ('1', '3', 'REGISTER_ADDR alice 127.0.0.1:5001 9255…'), ('1', '4', 'CREATE_GROUP movies alice'),
            ('2', '1', 'CREATE_USER bob pbkdf2-sha256$600000$…'), ('2', '2', 'LOGIN bob'),
            ('2', '3', 'JOIN_GROUP movies bob'), ('1', '5', 'ACCEPT_REQUEST movies bob')]
    b += text(60, 16, 'tracker', size=10.5, weight='600', color='#52606d')
    b += text(130, 16, 'number', size=10.5, weight='600', color='#52606d')
    b += text(170, 16, 'command (exactly what is replayed)', size=10.5, weight='600', color='#52606d', anchor='start')
    y = 24
    for t, n, c in rows:
        kind = 'tracker' if t == '1' else 'sec'
        b += box(30, y, 60, 22, t, kind, size=11.5, r=3)
        b += box(100, y, 60, 22, n, kind, size=11.5, r=3)
        b += box(170, y, 370, 22, c, 'white', size=10.5, r=3, bold_first=False)
        y += 26
    b += box(570, 30, 175, 74, ['Tracker 1 counts its own', 'operations 1, 2, 3, …', 'Tracker 2 counts its own.', ], 'plain', size=11)
    b += box(570, 122, 175, 96, ['Two jobs of this file:', '1. restart → replay it all', '2. the other tracker missed',
                                 '   some → read them back', '   and send them again'], 'plain', size=11)
    return svg(760, y + 6, b)


def fig_sync():
    return seq([('Tracker 1', 'tracker'), ('Tracker 2', 'tracker')], [
        ('note', 1, 'was stopped, or the network was cut'),
        ('note', 0, 'accepts operations 5 and 6: written to the oplog, cannot be sent'),
        ('band', 'the link comes back (Tracker 1 retries every second)', 'plain'),
        ('msg', 0, 1, 'TLS handshake — both show a certificate', 'sec'),
        ('msg', 0, 1, 'TRACKER_HELLO', 'tracker'),
        ('msg', 1, 0, 'HAVE 4      "I have your operations 1 to 4"', 'tracker'),
        ('note', 0, 'takes peer_mtx; reads its own operations after 4 from the oplog'),
        ('msg', 0, 1, 'OP 5 …', 'tracker'),
        ('msg', 0, 1, 'OP 6 …', 'tracker'),
        ('band', 'caught up — from here on, every new operation is forwarded at once', 'client'),
        ('msg', 0, 1, 'OP 7 …', 'tracker'),
        ('note', 1, 'a number it already applied is skipped, so sending twice is harmless'),
    ], pad=150)


def fig_failover():
    return seq([('Bob\'s client', 'client'), ('Tracker 2', 'tracker'), ('Tracker 1', 'tracker')], [
        ('note', 1, 'Tracker 2 stops'),
        ('msg', 0, 1, 'LIST_GROUPS      ✗ connection is dead', 'bad', True),
        ('note', 0, 'prints "Lost connection to tracker, switching..."'),
        ('msg', 0, 2, 'connect + TLS (is it really a tracker?)', 'sec'),
        ('msg', 0, 2, 'LOGIN bob ••••        (password kept in memory)', 'client'),
        ('msg', 0, 2, 'REGISTER_ADDR bob 127.0.0.1:5002 <key>', 'client'),
        ('msg', 0, 2, 'LIST_GROUPS      (the request that failed)', 'client'),
        ('msg', 2, 0, 'movies / END', 'tracker'),
        ('note', 0, 'the user only saw the "switching" line; running downloads were not touched'),
    ], pad=110)


def fig_trust():
    b = ''
    b += box(270, 8, 220, 44, ['ca_cert.pem', 'given to every client by a safe route'], 'sec', size=11.5)
    b += arrow(380, 54, 380, 74, 'sec')
    b += box(230, 76, 300, 44, ['"This connection really goes to a tracker"', 'checked in the TLS handshake'], 'tracker', size=11.5)
    xs = [130, 380, 630]
    for x in xs:
        b += arrow(380 + (x - 380) * 0.25, 122, x, 148, 'plain', width=1.2)
    b += box(20, 150, 220, 44, ['the piece hashes are real', 'they came from the tracker'], 'plain', size=11)
    b += box(270, 150, 220, 44, ['the peers\' key fingerprints are real', 'they came from the tracker'], 'plain', size=11)
    b += box(520, 150, 220, 44, ['"is this key a group member?"', 'answered by the tracker'], 'plain', size=11)
    for x in xs:
        b += arrow(x, 196, x, 216, 'plain', width=1.2)
    b += box(20, 218, 220, 44, ['a piece that matches its hash', 'is the real piece'], 'client', size=11)
    b += box(270, 218, 220, 44, ['a peer that proves that key', 'is the real peer'], 'client', size=11)
    b += box(520, 218, 220, 44, ['only members of the group', 'are given pieces'], 'client', size=11)
    return svg(760, 270, b)


def fig_mitm():
    b = ''
    b += box(30, 34, 130, 44, ['Client'], 'client', size=13)
    b += box(315, 20, 130, 72, ['Attacker', 'in the middle'], 'bad', size=13)
    b += box(600, 34, 130, 44, ['Tracker', 'or a peer'], 'tracker', size=13)
    b += arrow(164, 56, 311, 56, 'plain', both=True)
    b += arrow(449, 56, 596, 56, 'plain', both=True)
    items = [('Can she read it?', 'now: no — it is encrypted'),
             ('Can she change it?', 'now: no — every record has a check value'),
             ('Can she pretend to be the other side?', 'now: no — she cannot prove the key')]
    x = 20
    for q, a in items:
        b += box(x, 112, 236, 46, [q, a], 'plain', size=11.2)
        x += 242
    return svg(760, 166, b)


def fig_access():
    return seq([('Bob (downloader)', 'client'), ('Alice (seeder)', 'client'), ('Tracker', 'tracker')], [
        ('msg', 0, 1, 'TLS handshake: each side proves a key', 'sec'),
        ('note', 0, 'Alice\'s key = the fingerprint the tracker gave me ✓'),
        ('msg', 0, 1, 'GET_PIECE movie.bin 2 movies', 'client'),
        ('note', 1, 'do I share movie.bin in "movies"? ✓'),
        ('msg', 1, 2, 'CHECK_ACCESS movies <Bob\'s key>', 'sec'),
        ('msg', 2, 1, 'Allowed', 'tracker'),
        ('note', 1, 'remember this "yes" for 30 seconds'),
        ('msg', 1, 0, '00 03 D6 20 + 251,424 bytes', 'data'),
        ('note', 0, 'length ✓   SHA-1 = hash from the tracker ✓   → write to file'),
    ], pad=110)


def fig_password():
    b = ''
    b += box(10, 14, 170, 50, ['Client sends', 'login alice pw1', '(inside TLS)'], 'client', size=11)
    b += arrow(184, 39, 212, 39, 'plain')
    b += box(216, 14, 200, 50, ['Tracker hashes it', 'alice\'s salt + 600,000 rounds', 'before taking the lock'], 'sec', size=11)
    b += arrow(420, 39, 448, 39, 'plain')
    b += box(452, 14, 140, 50, ['Compare', 'with the stored hash'], 'tracker', size=11)
    b += arrow(596, 39, 624, 39, 'plain')
    b += box(628, 14, 122, 50, ['Login successful', 'or Login failed'], 'plain', size=11)
    b += text(10, 96, 'What is kept, and where:', size=11.5, anchor='start', weight='600')
    cells = [('tracker memory', 'the hash'), ('oplog.txt', 'CREATE_USER alice <hash>   and   LOGIN alice'),
             ('other tracker', 'the same two lines'), ('tracker screen', 'LOGIN alice ****')]
    widths = [130, 300, 150, 150]
    x = 10
    for (a, c), w in zip(cells, widths):
        b += box(x, 106, w, 42, [a, c], 'plain', size=10.8)
        x += w + 7
    return svg(760, 156, b)


def fig_timeline():
    b = ''
    items = [('v1', 'First submission', 'one piece at a time,\nbasic sync', 'plain'),
             ('v2', 'Correctness', 'one request/reply path,\ntimeouts, permissions', 'tracker'),
             ('v3', 'Parallel engine', 'thread pool, bitfields,\nrarest first', 'data'),
             ('v4', 'Robust sync', 'sequence numbers,\ncatch-up, failover', 'client'),
             ('v5', 'Security', 'TLS, password hashes,\naccess check', 'sec')]
    x = 12
    for i, (v, t, d, kind) in enumerate(items):
        lines = [v + ' — ' + t] + d.split('\n')
        b += box(x, 10, 136, 64, lines, kind, size=10.8)
        if i < len(items) - 1:
            b += arrow(x + 138, 42, x + 148, 42, 'plain', width=1.3)
        x += 150
    return svg(760, 84, b)


FIGS = dict(architecture=fig_architecture, pieces=fig_pieces, framing=fig_framing, rarest=fig_rarest,
            piece_flow=fig_piece_flow, threads=fig_threads, oplog=fig_oplog, sync=fig_sync, failover=fig_failover,
            trust=fig_trust, mitm=fig_mitm, access=fig_access, password=fig_password, timeline=fig_timeline)

_fig_no = [0]


def figure(name, caption):
    _fig_no[0] += 1
    return '<figure>%s<figcaption><b>Figure %d.</b> %s</figcaption></figure>' % (FIGS[name](), _fig_no[0], caption)


# ------------------------------------------------------------------ text

body = open(os.path.join(HERE, 'report_body.html')).read()
import re
body = re.sub(r'\{\{fig:(\w+)\|(.*?)\}\}', lambda m: figure(m.group(1), m.group(2)), body, flags=re.S)
# the first row of every table becomes a real header, so it repeats if the table is split over pages
body = re.sub(r'(<table[^>]*>)\s*(<tr><th.*?</tr>)', r'\1<thead>\2</thead>', body, flags=re.S)
css = open(os.path.join(HERE, 'report.css')).read()
html = '<!doctype html><html lang="en"><head><meta charset="utf-8"><title>Technical Report — P2P File Sharing System</title><style>%s</style></head><body>%s</body></html>' % (css, body)
open(os.path.join(HERE, 'technical_report.html'), 'w').write(html)
print('figures:', _fig_no[0])
