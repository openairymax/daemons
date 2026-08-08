#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
airy_mock_mcp_server.py — 模拟 MCP stdio server（P2-4 回归测试用）

通过 stdin/stdout 以 LSP 风格帧（Content-Length 头 + \\r\\n\\r\\n + JSON body）
与 MCP 客户端通信，支持：
  - initialize（返回 protocolVersion 2024-11-05 + capabilities.tools）
  - notifications/initialized（通知，不响应）
  - tools/list（返回 mock_echo 工具）
  - tools/call（mock_echo 返回 {"content":[{"type":"text","text":"echo:<arg>"}]}）
  - ping

用法：python3 airy_mock_mcp_server.py
"""

import json
import sys

TOOLS = [
    {
        "name": "mock_echo",
        "description": "Echo the given argument back to the caller",
        "inputSchema": {
            "type": "object",
            "properties": {"arg": {"type": "string"}},
            "required": ["arg"],
        },
    }
]


def read_frame():
    """读取一帧；EOF 时返回 None。"""
    headers = {}
    while True:
        line = sys.stdin.buffer.readline()
        if line == b"":
            return None  # EOF
        if line in (b"\r\n", b"\n"):
            break
        try:
            text = line.decode("utf-8").strip()
        except UnicodeDecodeError:
            continue
        if ":" in text:
            key, _, value = text.partition(":")
            headers[key.strip().lower()] = value.strip()
    try:
        n = int(headers.get("content-length", "0"))
    except ValueError:
        n = 0
    body = sys.stdin.buffer.read(n) if n > 0 else b""
    return body.decode("utf-8", errors="replace")


def send_frame(obj):
    body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    header = ("Content-Length: %d\r\n\r\n" % len(body)).encode("utf-8")
    sys.stdout.buffer.write(header + body)
    sys.stdout.buffer.flush()


def handle_request(req):
    method = req.get("method", "")
    rid = req.get("id")
    params = req.get("params") or {}

    if method == "initialize":
        return {
            "jsonrpc": "2.0",
            "id": rid,
            "result": {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "airy-mock-mcp", "version": "1.0.0"},
            },
        }
    if method == "notifications/initialized":
        return None  # 通知：不响应
    if method == "ping":
        return {"jsonrpc": "2.0", "id": rid, "result": {}}
    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": rid, "result": {"tools": TOOLS}}
    if method == "tools/call":
        name = params.get("name", "")
        args = params.get("arguments") or {}
        if name == "mock_echo":
            arg = args.get("arg", "")
            return {
                "jsonrpc": "2.0",
                "id": rid,
                "result": {"content": [{"type": "text", "text": "echo:" + str(arg)}]},
            }
        return {
            "jsonrpc": "2.0",
            "id": rid,
            "error": {"code": -32601, "message": "Tool not found: %s" % name},
        }
    return {
        "jsonrpc": "2.0",
        "id": rid,
        "error": {"code": -32601, "message": "Method not found: %s" % method},
    }


def main():
    while True:
        req_text = read_frame()
        if req_text is None:
            break
        if not req_text.strip():
            continue
        try:
            req = json.loads(req_text)
        except json.JSONDecodeError:
            continue
        resp = handle_request(req)
        if resp is not None:
            send_frame(resp)


if __name__ == "__main__":
    main()
