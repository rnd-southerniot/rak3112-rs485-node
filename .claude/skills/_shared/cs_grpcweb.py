# Minimal ChirpStack gRPC-web client (no REST API exists on this host).
import os, struct, urllib.request
from chirpstack_api import api
BASE = os.environ["CS_BASE"]; UA = os.environ.get("UA", "Mozilla/5.0")

def call(path, req, resp_cls, jwt=None):
    payload = req.SerializeToString()
    frame = b"\x00" + struct.pack(">I", len(payload)) + payload
    h = {"Content-Type": "application/grpc-web+proto", "X-Grpc-Web": "1",
         "Accept": "application/grpc-web+proto", "User-Agent": UA}
    if jwt: h["authorization"] = f"Bearer {jwt}"
    body = urllib.request.urlopen(
        urllib.request.Request(BASE + path, data=frame, headers=h, method="POST"), timeout=20).read()
    off, msg = 0, None
    while off < len(body):
        flag = body[off]; ln = struct.unpack(">I", body[off+1:off+5])[0]; off += 5
        chunk = body[off:off+ln]; off += ln
        if not (flag & 0x80): msg = resp_cls.FromString(chunk)   # 0x80 = trailer frame
    return msg

def auth():
    if os.environ.get("CS_API_TOKEN"): return os.environ["CS_API_TOKEN"]
    return call("/api.InternalService/Login",
                api.LoginRequest(email=os.environ["CS_ADMIN_USER"], password=os.environ["CS_ADMIN_PASS"]),
                api.LoginResponse).jwt
