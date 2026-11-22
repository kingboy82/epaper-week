#!/usr/bin/env python3
"""CalDAV 探针：查清"索引里有、但 GET 返回 404"的那几条事件到底是什么情况。

用法：
  1. 把企微邮箱和日历密码写进 bridge/config.json 的 caldav 段
  2. python3 tools/caldav_probe.py

输出会列出本周每个事件的 HTTP 状态，并对 404 的再做几种试探，
判断是资源真的不存在、还是需要不同的请求方式。
"""
import base64
import json
import sys
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from datetime import date, datetime, timedelta
from pathlib import Path

NS = {"d": "DAV:", "c": "urn:ietf:params:xml:ns:caldav"}
HOST = "https://caldav.wecom.work"


def make_client(cfg):
    auth = "Basic " + base64.b64encode(
        f"{cfg['user']}:{cfg['password']}".encode()).decode()

    def call(method, url, body=None, depth=None, extra_headers=None):
        headers = {"Authorization": auth,
                   "Content-Type": "application/xml; charset=utf-8"}
        if depth is not None:
            headers["Depth"] = depth
        if extra_headers:
            headers.update(extra_headers)
        req = urllib.request.Request(
            url, data=body.encode() if isinstance(body, str) else body,
            headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace")
        except Exception as e:
            return -1, str(e)

    return call


def hrefs(xml):
    out = []
    for m in ("<d:href>", "<href>"):
        i = 0
        while True:
            a = xml.find(m, i)
            if a < 0:
                break
            b = xml.find("</", a)
            out.append(xml[a + len(m):b].strip())
            i = b
    return out


def main():
    cfg_path = Path(__file__).resolve().parent.parent / "bridge" / "config.json"
    cfg = json.load(open(cfg_path))
    ca = cfg.get("caldav") or {}
    if not ca.get("user") or not ca.get("password"):
        print("请先在 bridge/config.json 的 caldav 段填 user / password")
        return 1

    call = make_client(ca)
    today = date.today()
    monday = today - timedelta(days=today.weekday())
    sunday = monday + timedelta(days=7)
    print(f"查询区间：{monday} ~ {sunday}\n")

    status, xml = call("PROPFIND", f"{HOST}/calendar/", """<?xml version="1.0"?>
<d:propfind xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav">
  <d:prop><d:resourcetype/><d:displayname/></d:prop></d:propfind>""", depth="1")
    print(f"PROPFIND /calendar/ -> {status}")

    cals = [h for h in hrefs(xml) if h.endswith("/") and h.rstrip("/") != "/calendar"]
    print(f"发现 {len(cals)} 个日历集合\n")

    query = f"""<?xml version="1.0"?>
<c:calendar-query xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav">
  <d:prop><d:getetag/><c:calendar-data/></d:prop>
  <c:filter><c:comp-filter name="VCALENDAR"><c:comp-filter name="VEVENT">
    <c:time-range start="{monday:%Y%m%d}T000000Z" end="{sunday:%Y%m%d}T000000Z"/>
  </c:comp-filter></c:comp-filter></c:filter></c:calendar-query>"""

    problems = []
    for cal in cals:
        url = urllib.parse.urljoin(HOST, cal)
        st, xml = call("REPORT", url, query, depth="1")
        items = [h for h in hrefs(xml) if not h.endswith("/")]
        if not items:
            continue
        print(f"\n── {cal}  REPORT={st}  事件 {len(items)} 个")
        for href in items:
            eurl = urllib.parse.urljoin(HOST, href)
            gs, body = call("GET", eurl)
            summary = ""
            if "SUMMARY" in body:
                for line in body.splitlines():
                    if line.startswith("SUMMARY:"):
                        summary = line[8:].strip()
                        break
            flag = "ok " if gs == 200 else "!! "
            print(f"  {flag}{gs}  {summary or '(无正文)'}  {href.split('/')[-1]}")
            if gs != 200:
                problems.append((eurl, href, gs))

    if not problems:
        print("\n没有异常事件。")
        return 0

    print(f"\n=== 对 {len(problems)} 个异常地址做试探 ===")
    for eurl, href, gs in problems:
        print(f"\n{href}  （GET {gs}）")
        for label, method, url, extra in [
            ("带 Accept: text/calendar", "GET", eurl, {"Accept": "text/calendar"}),
            ("PROPFIND Depth 0 看是否可枚举", "PROPFIND", eurl, None),
            ("去掉 .ics 后缀", "GET", eurl.replace(".ics", ""), None),
            ("该资源所在集合 PROPFIND", "PROPFIND",
             eurl.rsplit("/", 1)[0] + "/", None),
        ]:
            st, body = call(method, url, None, depth="0" if method == "PROPFIND" else None,
                            extra_headers=extra)
            note = ""
            if method == "PROPFIND" and st == 207:
                note = "（资源存在，只是 GET 被拒）"
            print(f"  {label:34s} -> {st}{note}")

    print("\n判读：")
    print("  · PROPFIND 207 而 GET 404 → 资源存在但正文不开放，多半是私密/仅参与者可见")
    print("  · 全部 404 → 服务端索引里的死链，事件可能已删除或改期")
    return 0


if __name__ == "__main__":
    sys.exit(main())
