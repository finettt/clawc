import http.server,json,threading,time
lock=threading.Lock(); active=0; maximum=0
class H(http.server.BaseHTTPRequestHandler):
 def log_message(self,*a):pass
 def do_POST(self):
  global active,maximum
  self.rfile.read(int(self.headers['Content-Length']))
  with lock: active+=1; maximum=max(maximum,active); open('/tmp/swarm-peak','w').write(str(maximum))
  time.sleep(.05)
  body=json.dumps({'choices':[{'message':{'role':'assistant','content':json.dumps({'report':'ok','messages':[]})}}]}).encode()
  self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
  with lock: active-=1
http.server.ThreadingHTTPServer(('127.0.0.1',19998),H).serve_forever()
