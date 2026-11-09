import json, http.server, time
class H(http.server.BaseHTTPRequestHandler):
 def log_message(self,*a):pass
 def do_POST(self):
  j=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
  text=[x['content'] for x in j['messages'] if x['role']=='user'][-1]
  if 'You are swarm agent alice.' in text and 'Message from user' in text:
   v={'report':'alice received','messages':[{'to':'@bob','text':'direct from alice'}]}
  elif 'You are swarm agent bob.' in text and 'direct from alice' in text:
   v={'report':'bob received direct','messages':[{'to':'#team','text':'group from bob'}]}
  elif 'You are swarm agent alice.' in text and 'group from bob' in text:
   v={'report':'alice received group','messages':[]}
  else: v={'report':'unexpected','messages':[]}
  body=json.dumps({'choices':[{'message':{'role':'assistant','content':json.dumps(v)}}]}).encode()
  self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
http.server.ThreadingHTTPServer(('127.0.0.1',19998),H).serve_forever()
