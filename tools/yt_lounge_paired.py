#!/usr/bin/env python3
# Option B (native app path): pair with the TV's native YouTube app via its
# "Link with TV code", then play a video through the Lounge API. This drives the
# full native YouTube TV app, exactly like a phone.
#
#   ./yt_lounge_paired.py <videoId> <pairing-code (spaces ok)>
import sys, json, time, random, urllib.parse, urllib.request

VIDEO = sys.argv[1]
CODE  = "".join(sys.argv[2:]).replace(" ", "")
SENDER = "YTC"
LOUNGE = "https://www.youtube.com/api/lounge"
UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/131 Safari/537.36"

def post(url, data):
    body = urllib.parse.urlencode(data).encode()
    h = {"User-Agent": UA, "Content-Type": "application/x-www-form-urlencoded",
         "Origin": "https://www.youtube.com"}
    return urllib.request.urlopen(urllib.request.Request(url, body, h), timeout=15)

# 1) exchange the manual TV code for the NATIVE app's screen
r = post(f"{LOUNGE}/pairing/get_screen", {"pairing_code": CODE})
screen = json.loads(r.read())["screen"]
screen_id = screen["screenId"]
print("== paired screen:", screen.get("name"), "| screenId", screen_id[:20] + "...", file=sys.stderr)

# 2) lounge token for that screen
r = post(f"{LOUNGE}/pairing/get_lounge_token_batch", {"screen_ids": screen_id})
tok = json.loads(r.read())["screens"][0]["loungeToken"]
print("== loungeToken", tok[:24] + "...", file=sys.stderr)

DEVICE_ID = "%032x" % random.getrandbits(128)
_rid = [random.randint(1, 9999)]
def next_rid(): _rid[0] += 1; return str(_rid[0])
def qp(extra):
    base = {"device": "REMOTE_CONTROL", "mdx-version": "3", "ui": "1", "v": "2", "name": SENDER,
            "app": "youtube-desktop", "loungeIdToken": tok, "id": DEVICE_ID, "VER": "8", "CVER": "1",
            "zx": "%08x" % random.getrandbits(32), "t": "1"}
    base.update(extra); return urllib.parse.urlencode(base)

# 3) bind -> SID + gsessionid
r = post(f"{LOUNGE}/bc/bind?{qp({'RID': next_rid()})}", {"count": "0"})
raw = r.read().decode("utf-8", "replace")
import re
events = []; i = 0
while i < len(raw):
    m = re.match(r"(\d+)\n", raw[i:])
    if not m: break
    ln = int(m.group(1)); s = i + m.end()
    try: events.extend(json.loads(raw[s:s + ln]))
    except: pass
    i = s + ln
SID = GS = None
for _, ev in events:
    if ev[0] == "c": SID = ev[1]
    elif ev[0] == "S": GS = ev[1]
print("== SID", SID, "gsession", GS, file=sys.stderr)
if not (SID and GS):
    print("bind failed; raw:", raw[:300], file=sys.stderr); sys.exit(1)

# 4) setPlaylist -> load + play on the native app
cmd = {"count": "1", "ofs": "0", "req0__sc": "setPlaylist", "req0_videoId": VIDEO,
       "req0_currentTime": "0", "req0_currentIndex": "0", "req0_listId": "",
       "req0_audioOnly": "false", "req0_prioritizeMobileSenderPlaybackStateOnConnection": "true"}
r = post(f"{LOUNGE}/bc/bind?{qp({'RID': next_rid(), 'SID': SID, 'gsessionid': GS, 'AID': '0'})}", cmd)
print("== setPlaylist ->", r.status, file=sys.stderr)
print("\nSUCCESS: playing", VIDEO, "on the NATIVE app — check the Shield.", file=sys.stderr)
