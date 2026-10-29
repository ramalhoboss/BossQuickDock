# SPDX-License-Identifier: GPL-3.0-or-later
"""Same-host BossQuickDock API controls. Tokens remain in memory only."""
import ipaddress,json,socket,threading,time,urllib.error,urllib.parse,urllib.request
class ConsoleError(Exception):
 def __init__(self,message,status=0):super().__init__(message);self.status=status
class Cancelled(ConsoleError):pass
class NoRedirect(urllib.request.HTTPRedirectHandler):
 def redirect_request(self,*args,**kwargs):return None
class ConsoleClient:
 def __init__(self,host,port=8090,cancel=None,timeout=2.5):
  self.host=str(ipaddress.IPv4Address(host));self.port=int(port)
  if not 1024<=self.port<=65535:raise ValueError('Console HTTP port must be 1024..65535')
  self.base='http://%s:%d'%(self.host,self.port);self.cancel=cancel or threading.Event();self.timeout=timeout;self.token=None;self.opener=urllib.request.build_opener(urllib.request.ProxyHandler({}),NoRedirect())
 def check(self):
  if self.cancel.is_set():raise Cancelled('Console operation cancelled')
 def raw(self,path,post=False):
  self.check();headers={'Accept':'application/json'}
  if post:headers['X-HB-Token']=self.token or ''
  req=urllib.request.Request(self.base+path,data=b'' if post else None,headers=headers,method='POST' if post else 'GET')
  try:
   with self.opener.open(req,timeout=self.timeout) as response:body=response.read(65537)
  except urllib.error.HTTPError as e:
   try:detail=json.loads(e.read(4096)).get('error','')
   except Exception:detail=''
   raise ConsoleError(str(detail)[:200] or 'Console returned HTTP '+str(e.code),e.code) from None
  except (urllib.error.URLError,TimeoutError,OSError) as e:raise ConsoleError('Console unavailable or request timed out. Check its status before retrying.') from None
  self.check()
  if len(body)>65536:raise ConsoleError('Console response is too large')
  try:j=json.loads(body)
  except (ValueError,UnicodeError):raise ConsoleError('Console returned invalid JSON') from None
  if not isinstance(j,dict):raise ConsoleError('Console returned an invalid response')
  return j
 def status(self):
  j=self.raw('/api/status');token=j.pop('token',None)
  if isinstance(token,str) and len(token)==32 and all(ch in '0123456789abcdefABCDEF' for ch in token):self.token=token
  return {k:j.get(k) for k in ['version','state','device','paused','connected','detail','uptime_s']}
 def native(self):return self.raw('/api/native')
 def post(self,path):
  if not self.token:self.status()
  try:return self.raw(path,True)
  except ConsoleError as e:
   if e.status!=403:raise
   self.status();return self.raw(path,True)
 def pause(self,seconds):
  if self.cancel.wait(seconds):self.check()
 def snapshot(self):
  status=self.status();n=self.native();return dict(console=status,native={k:n.get(k) for k in ['active','error','mode','codec','destination','port','reason','sent_packets','send_errors','capture_retries','network_reopens']})
 def stop_live(self,timeout=10):
  n=self.native()
  if n.get('active') and n.get('mode')!='live':raise ConsoleError('A console recording is active. Stop it from the console panel first.')
  if n.get('active'):
   self.post('/api/native/stop');end=time.monotonic()+timeout
   while True:
    n=self.native()
    if not n.get('active'):break
    if time.monotonic()>=end:raise ConsoleError('Stop requested, but capture is still closing. Bluetooth was not started.')
    self.pause(.15)
  return {'message':'PS5 live audio stopped. Receiver remains listening.','stopped':True}
 def start_live(self,destination,udp_port=18098,timeout=12):
  address=ipaddress.IPv4Address(destination);udp_port=int(udp_port)
  if int(address)==0 or address.is_multicast or int(str(address).split('.')[0])==0 or str(address).endswith('.255') or not 1024<=udp_port<=65535:raise ValueError('Enter a unicast PC IPv4 address and UDP port 1024..65535')
  destination=str(address);n=self.native()
  if n.get('active'):
   if n.get('mode')=='live' and n.get('destination')==destination and n.get('port')==udp_port:return {'message':'PS5 is already sending to this receiver.','started':True,'already_active':True}
   raise ConsoleError('Another capture is active. Stop it before starting audio on this PC.')
  s=self.status()
  if not (s.get('paused') and s.get('state') in ['idle','disconnected']):self.post('/api/disconnect')
  end=time.monotonic()+timeout
  while True:
   s=self.status()
   if s.get('paused') and s.get('state') in ['idle','disconnected']:break
   if time.monotonic()>=end:raise ConsoleError('Bluetooth is still disconnecting. Live audio was not started.')
   self.pause(.15)
  self.check();query=urllib.parse.urlencode(dict(ip=destination,port=udp_port))
  self.post('/api/native/live?'+query)
  end=time.monotonic()+4
  while True:
   n=self.native()
   if n.get('error') or not n.get('active'):raise ConsoleError('Live audio could not start: '+str(n.get('reason','check console panel')))
   if n.get('mode')!='live' or n.get('destination')!=destination or n.get('port')!=udp_port:raise ConsoleError('Console capture changed during start. Check the console status.')
   if n.get('sent_packets',0)>0:return {'message':'PS5 live Opus started. Playback is confirmed by receiver counters.','started':True}
   if time.monotonic()>=end:return {'message':'PS5 live Opus requested; waiting for source audio.','started':True,'awaiting_data':True}
   self.pause(.15)
 def return_bluetooth(self,timeout=15):
  self.stop_live();self.check();current=self.status()
  if current.get('state')=='streaming':return {'message':'Bluetooth audio is already connected to '+str(current.get('device') or 'saved device')+'.','bluetooth':True,'connected':True,'already_active':True}
  self.post('/api/connect');end=time.monotonic()+timeout
  while True:
   s=self.status()
   if s.get('state')=='streaming':return {'message':'Bluetooth audio connected to '+str(s.get('device') or 'saved device')+'.','bluetooth':True,'connected':True}
   if s.get('state')=='error' or time.monotonic()>=end:return {'message':'Bluetooth connection requested. Console status: '+str(s.get('detail') or s.get('state') or 'waiting')+'.','bluetooth':True,'connected':False}
   self.pause(.2)
