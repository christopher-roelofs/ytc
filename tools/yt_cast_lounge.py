#!/usr/bin/env python3
# Option B POC (complete): launch the device's YouTube app via Cast, get its
# screenId, then drive playback through the YouTube Lounge API (get_lounge_token
# -> bind -> setPlaylist). This is how a phone "casts" YouTube.
#
#   ./yt_cast_lounge.py <cast-ip> <videoId>
import sys, ssl, socket, struct, json, threading, time, random, urllib.parse, urllib.request, re

IP, VIDEO = sys.argv[1], sys.argv[2]
SENDER = "YTC"

# ---------- Cast v2 (raw TLS + protobuf) ----------
def varint(v):
    o = b""
    while True:
        b = v & 0x7f; v >>= 7
        o += bytes([b | (0x80 if v else 0)])
        if not v: return o
def field_str(f, s): s = s.encode(); return bytes([(f<<3)|2]) + varint(len(s)) + s
def field_var(f, v): return bytes([(f<<3)|0]) + varint(v)
def cast_message(src, dst, ns, payload):
    return (field_var(1,0)+field_str(2,src)+field_str(3,dst)+field_str(4,ns)+field_var(5,0)+field_str(6,payload))
def rv(b,i):
    v=0; sh=0
    while True:
        c=b[i]; i+=1; v|=(c&0x7f)<<sh
        if not (c&0x80): return v,i
        sh+=7
def parse(b):
    i=0; ns=pl=src=""
    while i<len(b):
        tag,i=rv(b,i); f=tag>>3; wt=tag&7
        if wt==0: _,i=rv(b,i)
        elif wt==2:
            ln,i=rv(b,i); s=b[i:i+ln]; i+=ln
            if f==2: src=s.decode("utf-8","replace")
            elif f==4: ns=s.decode("utf-8","replace")
            elif f==6: pl=s.decode("utf-8","replace")
        else: break
    return ns,pl,src

sock = socket.create_connection((IP,8009), timeout=8)
ctx = ssl._create_unverified_context()
tls = ctx.wrap_socket(sock)
print(f"TLS connected to {IP}:8009", file=sys.stderr)

def send(src,dst,ns,payload):
    m = cast_message(src,dst,ns,payload)
    tls.sendall(struct.pack(">I",len(m))+m)
def recv():
    hdr=b""
    while len(hdr)<4:
        d=tls.recv(4-len(hdr))
        if not d: return None
        hdr+=d
    ln=struct.unpack(">I",hdr)[0]; buf=b""
    while len(buf)<ln:
        d=tls.recv(ln-len(buf))
        if not d: return None
        buf+=d
    return parse(buf)

CONN="urn:x-cast:com.google.cast.tp.connection"; HEART="urn:x-cast:com.google.cast.tp.heartbeat"
RECV="urn:x-cast:com.google.cast.receiver"; MDX="urn:x-cast:com.google.youtube.mdx"

send("sender-0","receiver-0",CONN,'{"type":"CONNECT"}')
send("sender-0","receiver-0",RECV,'{"type":"LAUNCH","appId":"233637DE","requestId":1}')

transport=None; screen_id=None
while screen_id is None:
    r=recv()
    if r is None: print("cast closed", file=sys.stderr); sys.exit(1)
    ns,pl,src=r
    j={}
    try: j=json.loads(pl)
    except: pass
    if ns==HEART and j.get("type")=="PING":
        send("sender-0",src or "receiver-0",HEART,'{"type":"PONG"}'); continue
    if ns==RECV and j.get("type")=="RECEIVER_STATUS" and not transport:
        for app in j.get("status",{}).get("applications",[]):
            if app.get("appId")=="233637DE":
                transport=app.get("transportId")
                print("== YouTube launched, transport", transport, file=sys.stderr)
        if transport:
            send("sender-0",transport,CONN,'{"type":"CONNECT"}')
            send("sender-0",transport,MDX,'{"type":"getMdxSessionStatus"}')
    if ns==MDX and j.get("type")=="mdxSessionStatus":
        screen_id=j.get("data",{}).get("screenId")
        print("== screenId", screen_id, file=sys.stderr)

# keep the cast connection alive (PONG) so the app stays open during lounge control
def keepalive():
    while True:
        try:
            r=recv()
            if r is None: return
            ns,pl,src=r
            if ns==HEART:
                try:
                    if json.loads(pl).get("type")=="PING": send("sender-0",src or "receiver-0",HEART,'{"type":"PONG"}')
                except: pass
        except Exception: return
threading.Thread(target=keepalive,daemon=True).start()

# ---------- YouTube Lounge API ----------
LOUNGE="https://www.youtube.com/api/lounge"
UA="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/131 Safari/537.36"
def http_post(url, data, headers=None):
    body=urllib.parse.urlencode(data).encode()
    h={"User-Agent":UA,"Content-Type":"application/x-www-form-urlencoded","Origin":"https://www.youtube.com"}
    if headers: h.update(headers)
    return urllib.request.urlopen(urllib.request.Request(url,body,h),timeout=15)

# 1) lounge token from the screenId
r=http_post(f"{LOUNGE}/pairing/get_lounge_token_batch", {"screen_ids":screen_id})
tok=json.loads(r.read())["screens"][0]["loungeToken"]
print("== loungeToken", tok[:24]+"...", file=sys.stderr)

DEVICE_ID = "%032x" % random.getrandbits(128)
# RID must be a monotonic counter across the whole session (bind, then each command),
# or the server rejects with RID_OUTSIDE_WINDOW.
_rid = [random.randint(1, 9999)]
def next_rid():
    _rid[0] += 1; return str(_rid[0])
def qp(extra):
    base={"device":"REMOTE_CONTROL","mdx-version":"3","ui":"1","v":"2","name":SENDER,
          "app":"youtube-desktop","loungeIdToken":tok,"id":DEVICE_ID,"VER":"8","CVER":"1",
          "zx":"%08x"%random.getrandbits(32),"t":"1"}
    base.update(extra); return urllib.parse.urlencode(base)

# 2) bind — establish a session; parse SID + gsessionid from the first event batch
r=http_post(f"{LOUNGE}/bc/bind?{qp({'RID':next_rid()})}", {"count":"0"})
raw=r.read().decode("utf-8","replace")
# format: repeated "<len>\n<json-array>"
events=[]
i=0
while i < len(raw):
    m=re.match(r"(\d+)\n", raw[i:])
    if not m: break
    ln=int(m.group(1)); start=i+m.end(); chunk=raw[start:start+ln]
    try: events.extend(json.loads(chunk))
    except: pass
    i=start+ln
SID=GS=None
for _,ev in events:
    if ev[0]=="c": SID=ev[1]
    elif ev[0]=="S": GS=ev[1]
print("== SID",SID," gsession",GS, file=sys.stderr)
if not SID or not GS:
    print("bind failed; raw:", raw[:300], file=sys.stderr); sys.exit(1)

# 3) setPlaylist — load + play the video on the TV's YouTube app
cmd={"count":"1","ofs":"0",
     "req0__sc":"setPlaylist","req0_videoId":VIDEO,"req0_currentTime":"0",
     "req0_currentIndex":"0","req0_listId":"","req0_audioOnly":"false","req0_prioritizeMobileSenderPlaybackStateOnConnection":"true"}
r=http_post(f"{LOUNGE}/bc/bind?{qp({'RID':next_rid(),'SID':SID,'gsessionid':GS,'AID':'0'})}", cmd)
print("== setPlaylist ->", r.status, file=sys.stderr)
print("\nSUCCESS: setPlaylist sent for", VIDEO, "— check the Shield.", file=sys.stderr)
time.sleep(2)
