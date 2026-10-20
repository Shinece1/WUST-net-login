"""本地模拟深澜认证服务器，仅用于测试 wust-auth 的认证流程。

    python mock_server.py 18080

行为：
  GET  /probe              -> 未认证时 302 跳门户；认证成功后 200 且正文含 baidu
  POST /api/account/login  -> 校验 username/password/nasId，返回 {"code":0,...}
  GET  /state              -> 返回服务器记录到的请求信息（用于核对 URL 编码）
  GET  /reset              -> 复位登录状态
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs

EXPECT_USER = "2023000001"
EXPECT_PASS = "MyP@ss&w=rd 1"

# 认证成功后真正能拿到的百度首页（带百度自己的特征注释）
REAL_BAIDU = (b'<!DOCTYPE html><!--STATUS OK--><html><head>'
              b'<meta http-equiv="Content-Type" content="text/html;charset=utf-8">'
              b'<title>\xe7\x99\xbe\xe5\xba\xa6\xe4\xb8\x80\xe4\xb8\x8b\xef\xbc\x8c'
              b'\xe4\xbd\xa0\xe5\xb0\xb1\xe7\x9f\xa5\xe9\x81\x93</title></head>'
              b'<body>baidu</body></html>')

# 未认证时门户给的 302 目标（真实深澜门户就是这个样子）
PORTAL_REDIRECT = ("http://59.68.177.9/srun_portal_pc?ac_id=1&theme=basic"
                   "&url=http://www.baidu.com/")

STATE = {"logged_in": False, "last_form": None, "last_ct": None, "last_method": None}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _send(self, code, body=b"", ctype="text/plain"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if body:
            self.wfile.write(body)
        self.close_connection = True

    def do_GET(self):
        if self.path.startswith("/probe"):
            # 未认证：像真实门户那样把“你请求的地址”当参数回填，或者直接 302 跳门户
            if STATE["logged_in"]:
                self._send(200, REAL_BAIDU, "text/html")
            else:
                self.send_response(302)
                self.send_header("Location", PORTAL_REDIRECT)
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                self.close_connection = True
        elif self.path == "/real":
            self._send(200, REAL_BAIDU, "text/html")
        elif self.path == "/portal200-baidu":
            # 旧版程序会误判成“已在线”的典型情况：
            # 200 页面、正文里含有 baidu、还有指向百度的 JS 跳转
            body = (b'<html><head><meta http-equiv="refresh" '
                    b'content="0;url=http://www.baidu.com/"></head><body>'
                    b'<form action="/srun_portal_pc" method="post">'
                    b'<input name="ac_id" value="1">'
                    b'<script>window.location.href="http://www.baidu.com/"</script>'
                    b'baidu</form></body></html>')
            self._send(200, body, "text/html")
        elif self.path == "/portal302-baidu":
            # 真实门户未认证时的响应：302 跳到门户自身，并把原地址当 url 参数带上
            self.send_response(302)
            self.send_header("Location",
                             "http://59.68.177.9/srun_portal_pc?ac_id=1&theme=basic"
                             "&url=http://www.baidu.com/")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
        elif self.path == "/portal200-clean":
            # 200 门户页，但没有 srun 关键字：只能靠“正文不像真站点”来识别
            self._send(200, b"<html><body>Welcome! baidu is blocked. Please log in."
                            b"<a href='http://www.baidu.com/'>continue</a></body></html>",
                       "text/html")
        elif self.path == "/redir-same":
            self.send_response(302)
            self.send_header("Location", "http://%s/real" % self.headers.get("Host", "127.0.0.1"))
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
        elif self.path == "/redir-other":
            self.send_response(302)
            self.send_header("Location", "http://59.68.177.9/portal/index.html")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
        elif self.path == "/empty204":
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
        elif self.path == "/state":
            self._send(200, json.dumps(STATE, ensure_ascii=False).encode("utf-8"),
                       "application/json;charset=UTF-8")
        elif self.path == "/reset":
            STATE.update({"logged_in": False, "last_form": None, "last_ct": None,
                          "last_method": None})
            self._send(200, b"reset")
        else:
            self._send(404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n).decode("utf-8")
        STATE["last_form"] = raw
        STATE["last_ct"] = self.headers.get("Content-Type")
        STATE["last_method"] = self.command
        q = parse_qs(raw, keep_blank_values=True)
        user = q.get("username", [""])[0]
        pwd = q.get("password", [""])[0]
        nas = q.get("nasId", [""])[0]
        if user == EXPECT_USER and pwd == EXPECT_PASS and nas == "1":
            STATE["logged_in"] = True
            resp = {"code": 0, "authMsg": "已登录成功", "nasId": "1"}
        else:
            resp = {"code": 1, "authMsg": "用户名或密码错误"}
        body = json.dumps(resp, ensure_ascii=False).encode("utf-8")
        self._send(200, body, "application/json;charset=UTF-8")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
    print("mock srun server on 127.0.0.1:%d" % port, flush=True)
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
