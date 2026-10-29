# SPDX-License-Identifier: GPL-3.0-or-later
"""BQD Opus UDP receiver. Clocks are not synchronized: no end-to-end latency claim."""
import collections, json, socket, struct, threading, time
MAGIC=b'BQDOPUS1'
HEADER=struct.Struct('<8sHHQIQQHHI')
def parse_packet(data):
 if len(data)<48:raise ValueError('Short datagram')
 magic,version,header,session,seq,pts,elapsed,n,frames,rate=HEADER.unpack_from(data)
 if magic!=MAGIC or version!=1 or header!=48 or rate!=48000 or not 0<=n<=1200 or len(data)!=48+n or (frames!=(480 if n else 0)):raise ValueError('Invalid datagram')
 if n==0:return session,seq,pts,elapsed,None
 p=data[48:]
 if p[0]>>3!=30 or p[0]&3 or not p[0]&4:raise ValueError('Unsupported Opus packet configuration')
 return session,seq,pts,elapsed,p
class OpusDecoder:
 def __init__(self):
  import av
  self.av=av;self.ctx=av.CodecContext.create('opus','r');self.ctx.extradata=b'OpusHead'+bytes([1,2])+struct.pack('<HIhB',0,48000,0,0)
  self.resampler=av.AudioResampler(format='s16',layout='stereo',rate=48000)
 def decode(self,payload):
  out=bytearray()
  for frame in self.ctx.decode(self.av.Packet(payload)):
   for f in self.resampler.resample(frame):out.extend(bytes(f.planes[0])[:f.samples*4])
  if len(out)!=1920:raise ValueError('Unexpected decoded frame size')
  return bytes(out)
class JitterPlayer:
 def __init__(self,buffer_ms=50,sink=lambda pcm:None,decoder_factory=OpusDecoder,on_reset=lambda:None):
  if buffer_ms not in range(20,201,10):raise ValueError('Buffer must be 20..200 ms in 10-ms steps')
  self.buffer_ms=buffer_ms;self.sink=sink;self.decoder_factory=decoder_factory;self.on_reset=on_reset;self.missing_streak=0;self.late_streak=0;self.highest=-1;self.decode_streak=0
  self.stats=collections.Counter();self.session=None;self.pending={};self.decoder=None;self.expected=None;self.armed=False;self.first=0;self.due=0;self.last_receive=0;self.last_played_pts=None;self.ending=None;self.finished=False
 def _rebuffer(self,now,reason):
  self.pending.clear();self.expected=None;self.armed=False;self.first=now;self.due=now;self.last_played_pts=None;self.missing_streak=0;self.late_streak=0;self.decode_streak=0;self.decoder=self.decoder_factory();self.stats[reason]+=1;self.on_reset()
 def accept(self,data,now):
  try:session,seq,pts,elapsed,payload=parse_packet(data)
  except ValueError:self.stats['invalid']+=1;return
  if payload is None:
   if session==self.session:self.ending=seq
   return
  if session!=self.session:
   # A delayed datagram from the preceding stream must not reset a new stream.
   if hasattr(self,'retired') and session in self.retired:self.stats['late']+=1;return
   if not hasattr(self,'retired'):self.retired=collections.deque(maxlen=8)
   if self.session is not None:self.retired.append(self.session)
   self.session=session;self._rebuffer(now,'session_rebuffers');self.highest=-1;self.last_receive=0;self.stats['sessions']+=1;self.ending=None;self.finished=False
  if self.finished:self.stats['late']+=1;return
  self.stats['received']+=1;self.stats['bytes']+=len(data)
  if self.last_receive and now-self.last_receive>.25 and (self.armed or self.pending):self._rebuffer(now,'gap_rebuffers')
  forward=seq>self.highest;self.highest=max(self.highest,seq)
  if self.armed and self.expected is not None and seq<self.expected:
   self.stats['late']+=1
   if forward:self.late_streak+=1
   if self.late_streak<3 or not forward:return
   self._rebuffer(now,'late_rebuffers')
  else:self.late_streak=0
  if seq in self.pending:self.stats['duplicates']+=1;return
  if not self.pending and not self.armed:self.first=now;self.expected=seq
  if not self.armed and self.expected is not None:self.expected=min(self.expected,seq)
  self.pending[seq]=(pts,payload);self.last_receive=now
  if len(self.pending)>max(12,self.buffer_ms//10+8):
   newest=max(self.pending);entry=self.pending[newest];self.stats['overflow']+=len(self.pending)-1;self._rebuffer(now,'overflow_rebuffers');self.pending[newest]=entry;self.expected=newest
 def tick(self,now):
  if self.armed and now-self.last_receive>.25:
   self._rebuffer(now,'stalls');return
  if not self.armed:
   if not self.pending or now-self.first<self.buffer_ms/1000:return
   self.armed=True;self.due=now
  if now-self.due>.08:
   # Bound scheduler debt rather than trying to play stale audio in a burst.
   if self.pending:
    first=min(self.pending);self.stats['resync_skips']+=max(0,first-self.expected);self.expected=first
   self.due=now;self.stats['scheduler_resyncs']+=1
  for _ in range(4):
   if self.ending is not None and self.expected>=self.ending:
    self.armed=False;self.finished=True;self.pending.clear();self.expected=None;self.stats['clean_ends']+=1;return
   if now+1e-8<self.due:return
   entry=self.pending.pop(self.expected,None)
   if entry:
    pts,payload=entry
    if self.last_played_pts is not None and pts-self.last_played_pts!=2400:self.stats['pts_gaps']+=1
    self.last_played_pts=pts
    self.missing_streak=0
    try:pcm=self.decoder.decode(payload);self.stats['decoded']+=1;self.decode_streak=0
    except Exception:
     self.stats['decode_errors']+=1;self.decode_streak+=1;pcm=bytes(1920)
     if self.decode_streak>=3:self.decoder=self.decoder_factory();self.decode_streak=0;self.stats['decoder_resets']+=1
   else:self.stats['missing']+=1;self.missing_streak+=1;self.last_played_pts=None;pcm=bytes(1920)
   self.sink(pcm);self.expected+=1;self.due+=.01
   if self.missing_streak>=3:self._rebuffer(now,'underrun_rebuffers');return
 def snapshot(self):return dict(self.stats,buffer_ms=self.buffer_ms,buffered_packets=len(self.pending),playing=self.armed,session=str(self.session) if self.session is not None else None)
class Receiver:
 def __init__(self,bind='0.0.0.0',port=18098,source='192.168.1.73',buffer_ms=50,sink=lambda pcm:None,on_reset=lambda:None):
  self.bind=bind;self.port=port;self.source=source;self.player=JitterPlayer(buffer_ms,sink,on_reset=on_reset);self.lock=threading.Lock();self.stop_event=threading.Event();self.thread=None;self.error='';self.listening=False;self.foreign=0;self.reconnects=0;self.started_at=time.monotonic()
 def start(self):
  if self.thread and self.thread.is_alive():raise RuntimeError('Already listening')
  self.stop_event.clear();self.thread=threading.Thread(target=self.run,daemon=True);self.thread.start()
 def run(self):
  delay=.25
  while not self.stop_event.is_set():
   try:
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
     if hasattr(socket,'SO_EXCLUSIVEADDRUSE'):sock.setsockopt(socket.SOL_SOCKET,socket.SO_EXCLUSIVEADDRUSE,1)
     sock.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,262144);sock.bind((self.bind,self.port));sock.settimeout(.002);self.listening=True;self.error=''
     while not self.stop_event.is_set():
      try:
       data,peer=sock.recvfrom(2048);delay=.25
       if self.source and peer[0]!=self.source:self.foreign+=1;continue
       with self.lock:self.player.accept(data,time.monotonic())
      except socket.timeout:pass
      with self.lock:self.player.tick(time.monotonic())
   except Exception as e:
    self.error=str(e);self.reconnects+=1
    with self.lock:self.player._rebuffer(time.monotonic(),'socket_rebuffers')
   finally:self.listening=False
   if self.stop_event.wait(delay):break
   delay=min(5,delay*2)
 def stop(self):
  self.stop_event.set()
  if self.thread:self.thread.join(timeout=2)
 def snapshot(self):
  with self.lock:j=self.player.snapshot()
  j.update(listening=self.listening,error=self.error,foreign=self.foreign,socket_retries=self.reconnects,observed_seconds=round(time.monotonic()-self.started_at,1),end_to_end_latency_measured=False);return j
