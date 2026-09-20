"""Sert la page de polar_doctor_web (18081) en y injectant un pilote, pour les captures.
   ?view=diagram|dyn|data|vmg|boat|help|live  &lang=fr|en   ; /slow retarde l'événement load."""
import http.server, urllib.request, urllib.parse, time, re, sys
import os
APP = os.environ.get('SHOT_APP', 'http://127.0.0.1:18081')
GIF = b'GIF89a\x01\x00\x01\x00\x80\x00\x00\xff\xff\xff\x00\x00\x00!\xf9\x04\x01\x00\x00\x00\x00,\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02D\x01\x00;'

DRIVER = """
<script>
(async()=>{
  const q=new URLSearchParams(location.search), v=q.get('view')||'diagram';
  const $=s=>document.querySelector(s);
  const wait=ms=>new Promise(r=>setTimeout(r,ms));
  await wait(900);
  const tab=n=>document.querySelector('header nav button[data-v="'+n+'"]').click();
  if(v==='data') tab('data');
  if(v==='vmg') tab('vmg');
  if(v==='boat') tab('cfg');
  if(v==='help') $('#help').click();
  if(v==='dyn'){ $('#dyn').checked=true; $('#dyn').dispatchEvent(new Event('change')); }
  if(v==='live'){
    $('#lvsrc').value='tcp'; $('#lvsrc').dispatchEvent(new Event('change'));
    await wait(200); $('#lvaddr').value='127.0.0.1:2700';
    $('#lvmain').value='GV'; $('#lvhead').value='Code0'; $('#lvsea').value='Belle';
    $('#lvmain').dispatchEvent(new Event('change'));
    if($('#lvbtn').dataset.on!=='1') $('#lvbtn').click();   // jamais d'arrêt : il enregistrerait
    await wait(2500);
  }
})();
</script>
"""

class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        p = urllib.parse.urlparse(self.path)
        if p.path == '/slow':
            time.sleep(6)
            self.send_response(200); self.send_header('Content-Type','image/gif'); self.end_headers()
            self.wfile.write(GIF); return
        if p.path == '/':
            q = urllib.parse.parse_qs(p.query)
            lang = (q.get('lang') or ['fr'])[0]
            html = urllib.request.urlopen(APP + '/').read().decode()
            pre = ("<script>try{localStorage.setItem('lang','%s');localStorage.setItem('theme','light');}catch(e){}</script>" % lang)
            html = html.replace('<head>', '<head>' + pre, 1)
            html = html.replace('</body>', DRIVER + '<img src="/slow" style="position:absolute;width:1px;height:1px;left:0;top:0"></body>', 1)
            b = html.encode()
            self.send_response(200); self.send_header('Content-Type','text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(b))); self.end_headers(); self.wfile.write(b); return
        self.proxy('GET')
    def do_POST(self): self.proxy('POST')
    def proxy(self, method):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0) or 0)) if method == 'POST' else None
        req = urllib.request.Request(APP + self.path, data=body, method=method)
        try: r = urllib.request.urlopen(req)
        except Exception as e:
            self.send_response(502); self.end_headers(); self.wfile.write(str(e).encode()); return
        d = r.read()
        self.send_response(r.status)
        self.send_header('Content-Type', r.headers.get('Content-Type','text/plain'))
        self.send_header('Content-Length', str(len(d))); self.end_headers(); self.wfile.write(d)
    def log_message(self, *a): pass

http.server.ThreadingHTTPServer(('127.0.0.1', int(os.environ.get('SHOT_PORT', '18090'))), H).serve_forever()
