"""Exercise the native HTTP reader against a local, hardware-free HTTP server."""
import http.server
import subprocess
import sys
import threading


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        if self.path == '/redirect':
            self.send_response(302)
            self.send_header('Location', '/ok')
            self.end_headers()
            return
        self.send_response(200)
        self.end_headers()
        try:
            if self.path == '/ok':
                self.wfile.write(b'<xml/>')
            elif self.path == '/large':
                for _ in range(33):
                    self.wfile.write(b'x' * (1024 * 1024))
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass


with http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        base = f'http://127.0.0.1:{server.server_port}'
        for path, result in [('ok', 'ok'), ('empty', 'fail'), ('redirect', 'fail'), ('large', 'fail')]:
            subprocess.run([sys.argv[1], f'{base}/{path}', result], check=True, timeout=25)
    finally:
        server.shutdown()
        thread.join()
