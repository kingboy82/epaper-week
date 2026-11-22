#!/usr/bin/env python3
"""周视图桥接服务。

职责：把企业微信日历（CalDAV）和天气合成一段 JSON，供墨水屏设备拉取。
设备端不做任何网络协议处理，只 GET /api/week。

只依赖标准库。启动：
    cp config.example.json config.json   # 填好账号和坐标
    python3 bridge.py config.json
"""
from __future__ import annotations

import calendar
import json
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from datetime import date, datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

NS = {
    "d": "DAV:",
    "c": "urn:ietf:params:xml:ns:caldav",
}

WMO = {
    0: "晴", 1: "晴", 2: "多云", 3: "阴", 45: "雾", 48: "雾",
    51: "小雨", 53: "小雨", 55: "小雨", 56: "小雨", 57: "小雨",
    61: "小雨", 63: "中雨", 65: "大雨", 66: "雨", 67: "雨",
    71: "小雪", 73: "雪", 75: "大雪", 77: "雪",
    80: "阵雨", 81: "阵雨", 82: "暴雨", 85: "阵雪", 86: "阵雪",
    95: "雷阵雨", 96: "雷阵雨", 99: "雷阵雨",
}


# ---------------------------------------------------------------- CalDAV
class CalDAV:
    def __init__(self, url: str, user: str, password: str):
        self.url = url.rstrip("/") + "/"
        self.auth = "Basic " + __import__("base64").b64encode(
            f"{user}:{password}".encode()).decode()

    def _req(self, method: str, url: str, body: str | None = None, depth: str | None = None):
        headers = {"Authorization": self.auth, "Content-Type": "application/xml; charset=utf-8"}
        if depth is not None:
            headers["Depth"] = depth
        req = urllib.request.Request(url, data=body.encode() if body else None,
                                     headers=headers, method=method)
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.read().decode("utf-8", "replace")

    def calendars(self) -> list[str]:
        body = """<?xml version="1.0"?>
<d:propfind xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav">
  <d:prop><d:resourcetype/><d:displayname/></d:prop>
</d:propfind>"""
        xml = self._req("PROPFIND", self.url, body, depth="1")
        root = ET.fromstring(xml.encode())
        out = []
        for resp in root.findall("d:response", NS):
            rtype = resp.find(".//d:resourcetype", NS)
            if rtype is None or rtype.find("c:calendar", NS) is None:
                continue
            href = resp.find("d:href", NS).text
            out.append(urllib.parse.urljoin(self.url, href))
        return out

    def events(self, cal_url: str, start: date, end: date) -> list[dict]:
        body = f"""<?xml version="1.0"?>
<c:calendar-query xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav">
  <d:prop><d:getetag/><c:calendar-data/></d:prop>
  <c:filter><c:comp-filter name="VCALENDAR">
    <c:comp-filter name="VEVENT">
      <c:time-range start="{start:%Y%m%d}T000000Z" end="{end:%Y%m%d}T000000Z"/>
    </c:comp-filter>
  </c:comp-filter></c:filter>
</c:calendar-query>"""
        xml = self._req("REPORT", cal_url, body, depth="1")
        root = ET.fromstring(xml.encode())
        out = []
        for resp in root.findall("d:response", NS):
            data = resp.find(".//c:calendar-data", NS)
            if data is not None and data.text:
                out.extend(parse_ics(data.text))
            else:
                href = resp.find("d:href", NS).text
                ics = self._req("GET", urllib.parse.urljoin(self.url, href))
                out.extend(parse_ics(ics))
        return out


def unfold(text: str) -> list[str]:
    lines, buf = [], ""
    for raw in text.replace("\r\n", "\n").split("\n"):
        if raw.startswith((" ", "\t")) and buf:
            buf += raw[1:]
        else:
            if buf:
                lines.append(buf)
            buf = raw
    if buf:
        lines.append(buf)
    return lines


TZ_OFFSET = {"asia/shanghai": 8, "china standard time": 8, "prc": 8,
             "utc": 0, "gmt": 0, "asia/beijing": 8}


def parse_dt(value: str, tzid: str | None) -> datetime | None:
    """统一换算到东八区的墙上时间。"""
    value = value.strip()
    if value.endswith("Z"):
        # UTC 的墙上时间 +8 才是北京时间
        return datetime.strptime(value[:-1], "%Y%m%dT%H%M%S") + timedelta(hours=8)
    if "T" not in value:
        return datetime.strptime(value, "%Y%m%d")
    dt = datetime.strptime(value, "%Y%m%dT%H%M%S")
    offset = TZ_OFFSET.get((tzid or "").lower())
    if offset is not None and offset != 8:
        dt += timedelta(hours=8 - offset)
    return dt


def parse_ics(text: str) -> list[dict]:
    out, cur, in_event = [], {}, False
    for line in unfold(text):
        if line == "BEGIN:VEVENT":
            in_event, cur = True, {}
            continue
        if line == "END:VEVENT":
            if in_event and cur.get("start") and cur.get("summary"):
                out.append(cur)
            in_event = False
            continue
        if not in_event or ":" not in line:
            continue
        head, _, val = line.partition(":")
        key = head.split(";")[0].upper()
        tzid = None
        m = re.search(r"TZID=([^;:]+)", head)
        if m:
            tzid = m.group(1)
        if key == "DTSTART":
            cur["start"] = parse_dt(val, tzid)
            cur["allday"] = "VALUE=DATE" in head
        elif key == "DTEND":
            cur["end"] = parse_dt(val, tzid)
        elif key == "SUMMARY":
            cur["summary"] = val.replace("\\,", ",").replace("\\n", " ").strip()
        elif key == "RRULE":
            cur["rrule"] = dict(kv.split("=", 1) for kv in val.split(";") if "=" in kv)
        elif key == "STATUS" and val.upper() == "CANCELLED":
            cur["cancelled"] = True
    return out


def occurs_on(ev: dict, day: date) -> bool:
    """判断事件是否落在某天（含简单的 RRULE 展开）。"""
    start = ev["start"]
    if start.date() == day:
        return True
    rule = ev.get("rrule")
    if not rule or start.date() > day:
        return False
    until = rule.get("UNTIL", "")[:8]
    if until and day.strftime("%Y%m%d") > until:
        return False
    freq = rule.get("FREQ", "").upper()
    interval = int(rule.get("INTERVAL", "1") or 1)
    delta = (day - start.date()).days
    if delta < 0:
        return False
    if freq == "DAILY":
        return delta % interval == 0
    if freq == "WEEKLY":
        weeks = delta // 7
        if weeks % interval:
            return False
        byday = rule.get("BYDAY")
        if not byday:
            return day.weekday() == start.weekday()
        codes = {"MO": 0, "TU": 1, "WE": 2, "TH": 3, "FR": 4, "SA": 5, "SU": 6}
        days = [codes[b[-2:]] for b in byday.split(",") if b[-2:] in codes]
        return day.weekday() in days
    return False


# ---------------------------------------------------------------- 天气
def fetch_weather(lat: float, lon: float) -> dict[date, tuple[str, int, int]]:
    q = urllib.parse.urlencode({
        "latitude": lat, "longitude": lon,
        "daily": "weather_code,temperature_2m_max,temperature_2m_min",
        "timezone": "Asia/Shanghai", "forecast_days": 8, "past_days": 7,
    })
    with urllib.request.urlopen(f"https://api.open-meteo.com/v1/forecast?{q}", timeout=15) as r:
        data = json.load(r)
    daily = data["daily"]
    out = {}
    for i, ds in enumerate(daily["time"]):
        d = datetime.strptime(ds, "%Y-%m-%d").date()
        out[d] = (WMO.get(daily["weather_code"][i], "阴"),
                  round(daily["temperature_2m_min"][i]),
                  round(daily["temperature_2m_max"][i]))
    return out


# ---------------------------------------------------------------- 组装
def build_week(cfg: dict) -> dict:
    today = datetime.now().date()
    monday = today - timedelta(days=today.weekday())
    days = [monday + timedelta(days=i) for i in range(7)]

    try:
        weather = fetch_weather(cfg["latitude"], cfg["longitude"])
    except Exception as exc:  # 天气挂了不该让整页空掉
        print("weather failed:", exc, file=sys.stderr)
        weather = {}

    evs: list[dict] = []
    ca = cfg.get("caldav") or {}
    if ca.get("url") and ca.get("user") and ca.get("password"):
        try:
            client = CalDAV(ca["url"], ca["user"], ca["password"])
            for cal in client.calendars():
                evs.extend(client.events(cal, days[0], days[-1] + timedelta(days=1)))
        except Exception as exc:
            print("caldav failed:", exc, file=sys.stderr)

    payload_days = []
    for d in days:
        todo = []
        seen = set()
        for ev in evs:
            if ev.get("cancelled") or not occurs_on(ev, d):
                continue
            key = (ev["summary"], ev["start"].strftime("%H:%M"))
            if key in seen:
                continue
            seen.add(key)
            todo.append(ev)
        todo.sort(key=lambda e: e["start"])
        w = weather.get(d, ("—", 0, 0))
        payload_days.append({
            "m": d.month, "d": d.day,
            "cond": w[0], "lo": w[1], "hi": w[2],
            "ev": [{"t": e["start"].strftime("%H:%M"), "n": shorten(e["summary"])}
                   for e in todo[:8]],
        })

    return {
        "month": today.month,
        "week": today.isocalendar().week,
        "today": today.day,
        "first_wd": days[0].replace(day=1).weekday(),
        "ndays": calendar.monthrange(today.year, today.month)[1],
        "updated": f"更新 {datetime.now():%m-%d %H:%M}",
        "days": payload_days,
    }


def shorten(name: str, limit: int = 10) -> str:
    name = re.sub(r"[（(].*?[）)]", "", name).strip()
    return name if len(name) <= limit else name[:limit]


# ---------------------------------------------------------------- HTTP
class Handler(BaseHTTPRequestHandler):
    cache = {"data": None, "at": 0.0}

    def do_GET(self):
        if self.path.split("?")[0] != "/api/week":
            self.send_error(404)
            return
        now = time.time()
        if self.cache["data"] is None or now - self.cache["at"] > 300:
            try:
                Handler.cache["data"] = build_week(self.server.cfg)
                Handler.cache["at"] = now
            except Exception as exc:
                second = f'{{"error":"{exc}"}}'.encode()
                self.send_response(503)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(second)))
                self.end_headers()
                self.wfile.write(second)
                return
        body = json.dumps(Handler.cache["data"], ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "config.json"
    with open(path) as f:
        cfg = json.load(f)
    port = int(cfg.get("port", 8787))
    srv = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    srv.cfg = cfg
    print(f"bridge listening on :{port}/api/week")
    srv.serve_forever()


if __name__ == "__main__":
    main()
